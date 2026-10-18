#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/UEAgentGraphUtils.h"

// Engine / editor
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "UObject/SavePackage.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"            // IFileManager::IsReadOnly -- a .uasset read-only on disk cannot be saved
#include "Serialization/ObjectWriter.h" // FObjectWriter / FObjectReader -- the in-memory snapshot a failed edit is put back from
#include "Serialization/ObjectReader.h"
#include "UObject/UObjectHash.h"        // GetObjectsWithOuter -- the components an edit added
#include "UObject/ObjectKey.h"          // TObjectKey
#include "Engine/CurveTable.h"          // UCurveTable + FCurveTableRowHandle (set_..._custom_curve FinalLookupCurve)
#include "Engine/World.h"               // GEditor->GetEditorWorldContext
#include "EngineUtils.h"                // TActorIterator
#include "Editor.h"                     // GEditor

// GameplayAbilities
#include "Abilities/GameplayAbility.h"   // UGameplayAbility, EditorGetAssetTags/GetAssetTags
#include "GameplayEffect.h"              // UGameplayEffect, FGameplayModifierInfo, EGameplayEffectDurationType, FGameplayEffectModifierMagnitude, FSetByCallerFloat, FAttributeBasedFloat
#include "GameplayEffectTypes.h"         // EGameplayModOp
#include "GameplayEffectAttributeCaptureDefinition.h" // EGameplayEffectAttributeCaptureSource, FGameplayEffectAttributeCaptureDefinition
#include "GameplayModMagnitudeCalculation.h" // UGameplayModMagnitudeCalculation (custom magnitude calc base)
#include "ScalableFloat.h"               // FScalableFloat
#include "AttributeSet.h"                // UAttributeSet, FGameplayAttribute
#include "AbilitySystemComponent.h"      // UAbilitySystemComponent
#include "GameplayAbilitySpec.h"         // FGameplayAbilitySpec

// GameplayCueNotify base classes — both Blueprintable, MinimalAPI.
// We only set their public GameplayCueTag UPROPERTY on the CDO (no exported method calls).
#include "GameplayCueNotify_Static.h"   // UGameplayCueNotify_Static (UObject)
#include "GameplayCueNotify_Actor.h"    // AGameplayCueNotify_Actor (AActor)
#include "GameplayCueNotify_Looping.h"  // AGameplayCueNotify_Looping (modern looping notify)
#include "GameplayCueNotify_Burst.h"    // UGameplayCueNotify_Burst (modern one-shot notify)

// GameplayEffectComponents — MinimalAPI UClasses derived from
// UGameplayEffectComponent. We add them to the GE CDO via the header-inline
// UGameplayEffect::AddComponent<T>() template, and read GEComponents (protected)
// back via reflection.
#include "GameplayEffectComponent.h"
#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/AdditionalEffectsGameplayEffectComponent.h"
#include "GameplayEffectComponents/ChanceToApplyGameplayEffectComponent.h"
#include "GameplayEffectComponents/ImmunityGameplayEffectComponent.h"
// The remaining 6 GameplayEffectComponent subclasses (config setters +
// the add_gameplay_effect_component type map). All MinimalAPI; their
// SetAndApply*/Set*/AddGrantedAbilityConfig mutators carry GAMEPLAYABILITIES_API.
#include "GameplayEffectComponents/TargetTagRequirementsGameplayEffectComponent.h"
#include "GameplayEffectComponents/BlockAbilityTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/AbilitiesGameplayEffectComponent.h"
#include "GameplayEffectComponents/RemoveOtherGameplayEffectComponent.h"
#include "GameplayEffectComponents/CustomCanApplyGameplayEffectComponent.h"
#include "GameplayEffectComponents/CancelAbilityTagsGameplayEffectComponent.h"

// GameplayTags
#include "GameplayTagsManager.h"
#include "GameplayTagContainer.h"

// For iterating loaded UClass objects / properties
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

/**
 * GAS (Gameplay Ability System) asset-authoring commands.
 *
 * These commands AUTHOR persisted /Game assets -- Blueprints parented to GAS base classes (abilities, effects,
 * attribute sets, magnitude calculations, gameplay cue notifies) and the settings on their CDOs -- rather
 * than manipulating a live AbilitySystemComponent at runtime.
 *
 * Engine source:
 *   Engine/Plugins/Runtime/GameplayAbilities/Source/GameplayAbilities/Public/{GameplayEffect.h,
 *   Abilities/GameplayAbility.h, AttributeSet.h, ScalableFloat.h, GameplayEffectTypes.h}.
 */
namespace UEAgentGASAuthoringCommands
{

// MCP-DOMAIN: GameplayAbilities

namespace
{
    // The Blueprint a create_* command made, or found already there, at PackagePath/Name.
    struct FGASAuthoringCreatedBlueprint
    {
        FString Name;
        FString PackagePath;
        UClass* ParentClass = nullptr;
        UBlueprint* Blueprint = nullptr;
        bool bAlreadyExisted = false;
    };

    // Reads name and package_path (default /Game/Blueprints) into Out; the error reply when name is missing.
    TSharedPtr<FJsonObject> GASAuthoring_ReadCreateTarget(const TSharedPtr<FJsonObject>& Params, FGASAuthoringCreatedBlueprint& Out)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Out.Name, mcp_common::EAllowEmpty::Yes))
        {
            return Missing;
        }
        Out.PackagePath = TEXT("/Game/Blueprints");
        Params->TryGetStringField(TEXT("package_path"), Out.PackagePath);
        Out.PackagePath = mcp_common::NormalizeFolder(Out.PackagePath, TEXT("/Game/Blueprints"));
        return nullptr;
    }

    // The asset path Out names, PackagePath/Name, as CreateOrLoadBlueprint joins it.
    FString GASAuthoring_CreateTargetPath(const FGASAuthoringCreatedBlueprint& Out)
    {
        return FString::Printf(TEXT("%s/%s"), *Out.PackagePath, *Out.Name);
    }

    // Creates a UBlueprint of ParentClass at the Out.PackagePath/Out.Name that GASAuthoring_ReadCreateTarget read,
    // compiles and saves it. Idempotent: a Blueprint of ParentClass already at that path is loaded instead
    // (bAlreadyExisted). One of another class is an error, before anything touches it: the caller asked
    // for a ParentClass Blueprint and none exists there. The error reply on failure.
    TSharedPtr<FJsonObject> GASAuthoring_CreateOrLoadTarget(UClass* ParentClass, FGASAuthoringCreatedBlueprint& Out)
    {
        Out.ParentClass = ParentClass;
        FString Error;
        Out.Blueprint = mcp_common::CreateOrLoadBlueprint(ParentClass, Out.PackagePath, Out.Name, Out.bAlreadyExisted, Error);
        if (!Out.Blueprint)
        {
            return mcp_common::MakeErrorReply(Error);
        }
        const UClass* GeneratedClass = Out.Blueprint->GeneratedClass;
        if (Out.bAlreadyExisted && !(GeneratedClass && GeneratedClass->IsChildOf(ParentClass)))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("A Blueprint already at %s/%s is not a %s (its class: %s, parent: %s); nothing was created or changed. ")
                TEXT("Pick another name, or delete that asset first."),
                *Out.PackagePath, *Out.Name, *ParentClass->GetName(),
                GeneratedClass ? *GeneratedClass->GetName() : TEXT("none, it has no generated class"),
                Out.Blueprint->ParentClass ? *Out.Blueprint->ParentClass->GetName() : TEXT("none")));
        }
        return nullptr;
    }

    // GASAuthoring_ReadCreateTarget, then GASAuthoring_CreateOrLoadTarget: the whole create of a create_* command.
    TSharedPtr<FJsonObject> GASAuthoring_CreateBlueprint(const TSharedPtr<FJsonObject>& Params, UClass* ParentClass,
                                                         FGASAuthoringCreatedBlueprint& Out)
    {
        if (TSharedPtr<FJsonObject> Missing = GASAuthoring_ReadCreateTarget(Params, Out))
        {
            return Missing;
        }
        return GASAuthoring_CreateOrLoadTarget(ParentClass, Out);
    }

    // The fields every create_* reply starts with; IsKindField says whether the generated class derives
    // from the requested parent class.
    TSharedPtr<FJsonObject> GASAuthoring_CreatedBlueprintFields(const FGASAuthoringCreatedBlueprint& Created,
                                                                const TCHAR* ParentClassName, const TCHAR* IsKindField)
    {
        UClass* GeneratedClass = Created.Blueprint->GeneratedClass;
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("name"), Created.Name);
        Result->SetStringField(TEXT("asset_path"), FString::Printf(TEXT("%s/%s"), *Created.PackagePath, *Created.Name));
        Result->SetStringField(TEXT("parent_class"), ParentClassName);
        Result->SetStringField(TEXT("generated_class"), GeneratedClass ? GeneratedClass->GetName() : FString());
        Result->SetBoolField(IsKindField, GeneratedClass && GeneratedClass->IsChildOf(Created.ParentClass));
        return Result;
    }

    // The CDO of the Blueprint at BlueprintPath as a T. Null with OutError when no Blueprint loads there ("<Kind>
    // Blueprint not found"), it has no GeneratedClass, or its CDO is not a T.
    template <typename T>
    T* GASAuthoring_LoadBlueprintCDO(const FString& BlueprintPath, const TCHAR* Kind, UBlueprint*& OutBlueprint, FString& OutError)
    {
        OutBlueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
        if (!OutBlueprint)
        {
            OutError = FString::Printf(TEXT("%s Blueprint not found: %s"), Kind, *BlueprintPath);
            return nullptr;
        }
        if (!OutBlueprint->GeneratedClass)
        {
            OutError = FString::Printf(TEXT("Blueprint has no GeneratedClass: %s"), *BlueprintPath);
            return nullptr;
        }
        T* CDO = Cast<T>(OutBlueprint->GeneratedClass->GetDefaultObject());
        if (!CDO)
        {
            OutError = FString::Printf(TEXT("Blueprint is not a U%s: %s"), *T::StaticClass()->GetName(), *BlueprintPath);
        }
        return CDO;
    }

    // Load a GE blueprint by /Game path and return its CDO cast to UGameplayEffect.
    UGameplayEffect* LoadGameplayEffectCDO(const FString& BlueprintPath, UBlueprint*& OutBlueprint, FString& OutError)
    {
        return GASAuthoring_LoadBlueprintCDO<UGameplayEffect>(BlueprintPath, TEXT("GameplayEffect"), OutBlueprint, OutError);
    }

    // The GE modifier a modifier command addresses: the ge_blueprint_path and modifier_index it reads, and the GE
    // Blueprint and CDO they load.
    struct FGASAuthoringModifier
    {
        FString BlueprintPath;
        int32 Index = 0;
        UBlueprint* Blueprint = nullptr;
        UGameplayEffect* GECDO = nullptr;

        FGameplayEffectModifierMagnitude& Magnitude() const
        {
            return GECDO->Modifiers[Index].ModifierMagnitude;
        }
    };

    // Reads the required ge_blueprint_path and modifier_index into Out; the error reply when either is missing.
    TSharedPtr<FJsonObject> GASAuthoring_ReadModifierTarget(const TSharedPtr<FJsonObject>& Params, FGASAuthoringModifier& Out)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ge_blueprint_path"), Out.BlueprintPath, mcp_common::EAllowEmpty::Yes))
        {
            return Missing;
        }
        double ModifierIndexD = 0.0;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("modifier_index"), ModifierIndexD))
        {
            return Missing;
        }
        Out.Index = (int32)ModifierIndexD;
        return nullptr;
    }

    // The refusal of an edit of the package PackageName that its save could not persist, or null when the edit
    // may go ahead. SaveAssetPackage refuses every save while a Play-in-Editor session runs, and a save under an
    // enforced read-only root, and the engine cannot replace a .uasset that is read-only on disk (a Git LFS
    // `lockable` file nobody has locked); editing first would leave the change in memory, and its package dirty,
    // behind an error reply. The package need not exist yet: a new one has no file, so only the first two apply.
    TSharedPtr<FJsonObject> GASAuthoring_RefuseUnsavablePackage(const FString& PackageName)
    {
        if (mcp_common::IsPieActive())
        {
            return mcp_common::MakeErrorReply(TEXT("REFUSED: a Play-in-Editor session is running and assets cannot ")
                TEXT("be saved during it, so nothing was changed. Stop PIE (stop_play_in_editor) and retry."));
        }
        if (mcp_common::IsUnderReadOnlyRoot(PackageName) && mcp_common::IsReadOnlyRootSaveEnforcementEnabled())
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("REFUSED: '%s' lives under a read-only root, where saves are refused, so nothing was changed. ")
                TEXT("Fork the asset into a writable root, or set the console variable ")
                TEXT("'mcp.ReadOnlyRootEnforcement 0' if this really is intended."),
                *PackageName));
        }
        // The same file SaveAssetPackage writes. A package with no file yet has nothing read-only to replace.
        FString FileName;
        if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, FileName, FPackageName::GetAssetPackageExtension())
            && IFileManager::Get().FileExists(*FileName) && IFileManager::Get().IsReadOnly(*FileName))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("REFUSED: %s is read-only on disk, so it cannot be saved and nothing was changed. In a Git LFS ")
                TEXT("repo that is a `lockable` file you have not locked: take the lock (git lfs lock <file>) and retry."),
                *FileName));
        }
        return nullptr;
    }

    // GASAuthoring_RefuseUnsavablePackage for an edit of Blueprint. Call it after loading the Blueprint and before
    // the first write to it; a CDO edit whose save fails for any other reason is undone by FGASAuthoringCDOEdit.
    TSharedPtr<FJsonObject> GASAuthoring_RefuseUnsavableEdit(const UBlueprint* Blueprint)
    {
        return GASAuthoring_RefuseUnsavablePackage(Blueprint->GetOutermost()->GetName());
    }

    // The in-memory state a CDO edit starts from, so that an edit which fails, or whose save fails, leaves nothing
    // behind its error reply: otherwise the change stays in memory, its package dirty, and the next save of that
    // package writes it. Take it after GASAuthoring_RefuseUnsavableEdit and before the first write to the CDO.
    // It holds the properties of the CDO and of the GameplayEffectComponents it already has (a component setter
    // edits those), a GameplayEffect's cached tags, and the package's dirty flag. Undo() puts them back and takes
    // out any component the edit added; Keep() is called once the edit is saved. Destroyed without either, it
    // undoes, so every error return after it rolls the edit back.
    class FGASAuthoringCDOEdit
    {
    public:
        explicit FGASAuthoringCDOEdit(UObject* InCDO)
            : CDO(InCDO)
            , Package(InCDO->GetOutermost())
            , bPackageWasDirty(InCDO->GetOutermost()->IsDirty())
        {
            TArray<UObject*> Subobjects;
            GetObjectsWithOuter(InCDO, Subobjects, EGetObjectsFlags::None);
            Snapshot(InCDO);
            for (UObject* Subobject : Subobjects)
            {
                if (Subobject->IsA<UGameplayEffectComponent>())
                {
                    ExistingSubobjects.Add(Subobject);
                    Snapshot(Subobject);
                }
            }
            if (const UGameplayEffect* GE = Cast<UGameplayEffect>(InCDO))
            {
                // Plain members, not UPROPERTYs, so the archive does not carry them; the component setters'
                // SetAndApply* add to them.
                bHasCachedTags = true;
                CachedAssetTags = GE->CachedAssetTags;
                CachedGrantedTags = GE->CachedGrantedTags;
                CachedBlockedAbilityTags = GE->CachedBlockedAbilityTags;
            }
        }

        FGASAuthoringCDOEdit(const FGASAuthoringCDOEdit&) = delete;
        FGASAuthoringCDOEdit& operator=(const FGASAuthoringCDOEdit&) = delete;

        ~FGASAuthoringCDOEdit()
        {
            Undo();
        }

        // The edit was saved: nothing is put back.
        void Keep()
        {
            bSettled = true;
        }

        void Undo()
        {
            if (bSettled)
            {
                return;
            }
            bSettled = true;
            for (const FObjectState& State : States)
            {
                if (UObject* Object = State.Object.Get())
                {
                    FObjectReader(Object, State.Bytes);
                }
            }
            UObject* Owner = CDO.Get();
            if (UGameplayEffect* GE = Cast<UGameplayEffect>(Owner); GE && bHasCachedTags)
            {
                GE->CachedAssetTags = CachedAssetTags;
                GE->CachedGrantedTags = CachedGrantedTags;
                GE->CachedBlockedAbilityTags = CachedBlockedAbilityTags;
            }
            if (Owner)
            {
                // A component the edit created (UGameplayEffect::AddComponent, the only subobject these edits make)
                // is no longer referenced once the properties are back, but a save takes every RF_Public object of
                // the package as an export: it leaves the package, so no later save writes it.
                TArray<UObject*> Subobjects;
                GetObjectsWithOuter(Owner, Subobjects, EGetObjectsFlags::None);
                for (UObject* Subobject : Subobjects)
                {
                    if (Subobject->IsA<UGameplayEffectComponent>() && !ExistingSubobjects.Contains(Subobject))
                    {
                        Subobject->ClearFlags(RF_Public | RF_Standalone);
                        Subobject->Rename(nullptr, GetTransientPackage(),
                            REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional);
                        Subobject->MarkAsGarbage();
                    }
                }
            }
            if (UPackage* OwnerPackage = Package.Get())
            {
                OwnerPackage->SetDirtyFlag(bPackageWasDirty);
            }
        }

    private:
        struct FObjectState
        {
            TWeakObjectPtr<UObject> Object;
            TArray<uint8> Bytes;
        };

        void Snapshot(UObject* Object)
        {
            FObjectState& State = States.AddDefaulted_GetRef();
            State.Object = Object;
            // Without delta: a property still equal to its archetype's value would be left out of a delta and so
            // never put back after the edit changed it.
            FObjectWriter(Object, State.Bytes, /*bIgnoreClassRef*/ false, /*bIgnoreArchetypeRef*/ false, /*bDoDelta*/ false);
        }

        TWeakObjectPtr<UObject> CDO;
        TWeakObjectPtr<UPackage> Package;
        bool bPackageWasDirty = false;
        TArray<FObjectState> States;
        TSet<TObjectKey<UObject>> ExistingSubobjects;
        bool bHasCachedTags = false;
        FGameplayTagContainer CachedAssetTags;
        FGameplayTagContainer CachedGrantedTags;
        FGameplayTagContainer CachedBlockedAbilityTags;
        bool bSettled = false;
    };

    // LoadGameplayEffectCDO into Out for a modifier edit, refused when the CDO's Modifiers array has no entry at
    // Out.Index or when the edit could not be saved (GASAuthoring_RefuseUnsavableEdit); the error reply on failure.
    TSharedPtr<FJsonObject> GASAuthoring_LoadGEModifier(FGASAuthoringModifier& Out)
    {
        FString Error;
        Out.GECDO = LoadGameplayEffectCDO(Out.BlueprintPath, Out.Blueprint, Error);
        if (!Out.GECDO)
        {
            return mcp_common::MakeErrorReply(Error);
        }
        if (!Out.GECDO->Modifiers.IsValidIndex(Out.Index))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("modifier_index %d out of range (Modifiers has %d entries; call add_gameplay_effect_modifier first)"),
                Out.Index, Out.GECDO->Modifiers.Num()));
        }
        return GASAuthoring_RefuseUnsavableEdit(Out.Blueprint);
    }

    // A modifier setter's reply, starting with the modifier it addressed and its magnitude type.
    TSharedPtr<FJsonObject> GASAuthoring_ModifierResult(const FGASAuthoringModifier& Modifier, const TCHAR* MagnitudeType)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("ge_blueprint_path"), Modifier.BlueprintPath);
        Result->SetNumberField(TEXT("modifier_index"), Modifier.Index);
        Result->SetNumberField(TEXT("modifier_count"), Modifier.GECDO->Modifiers.Num());
        Result->SetStringField(TEXT("magnitude_type"), MagnitudeType);
        return Result;
    }

    // Reflection accessor for the protected inner magnitude sub-structs of
    // FGameplayEffectModifierMagnitude (AttributeBasedMagnitude / CustomMagnitude are
    // protected UPROPERTYs — reflection ignores access). The outer struct carries no
    // GAMEPLAYABILITIES_API, so its StaticStruct() is not safely callable cross-module;
    // resolve the UScriptStruct by name (NativeFirst) — the link-safe by-name pattern.
    // Returns a MUTABLE pointer into the live magnitude (used for both read-back and the
    // FinalLookupCurve write), or nullptr if reflection is unavailable.
    template <typename T>
    T* GetMagnitudeSubStructPtr(FGameplayEffectModifierMagnitude& Magnitude, const TCHAR* FieldName)
    {
        UScriptStruct* MagStruct = FindFirstObject<UScriptStruct>(
            TEXT("GameplayEffectModifierMagnitude"), EFindFirstObjectOptions::NativeFirst);
        if (!MagStruct)
        {
            return nullptr;
        }
        FStructProperty* Prop = FindFProperty<FStructProperty>(MagStruct, FieldName);
        if (!Prop)
        {
            return nullptr;
        }
        return Prop->ContainerPtrToValuePtr<T>(&Magnitude);
    }

    // The name of the first attribute Magnitude captures, or empty when it captures none.
    FString GASAuthoring_CapturedAttributeName(const FGameplayEffectModifierMagnitude& Magnitude)
    {
        TArray<FGameplayEffectAttributeCaptureDefinition> CaptureDefs;
        Magnitude.GetAttributeCaptureDefinitions(CaptureDefs);
        return CaptureDefs.Num() > 0 ? CaptureDefs[0].AttributeToCapture.GetName() : FString();
    }

    // Reads the required curve_table_path and row_name of a curve setter and loads that UCurveTable into
    // OutCurveTable; the error reply when either is missing or the path is not a curve table.
    TSharedPtr<FJsonObject> GASAuthoring_ReadCurveTableRow(const TSharedPtr<FJsonObject>& Params, UCurveTable*& OutCurveTable, FString& OutRowName)
    {
        FString CurveTablePath;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("curve_table_path"), CurveTablePath))
        {
            return Missing;
        }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("row_name"), OutRowName))
        {
            return Missing;
        }
        OutCurveTable = Cast<UCurveTable>(StaticLoadObject(UCurveTable::StaticClass(), nullptr, *CurveTablePath));
        if (!OutCurveTable)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("curve_table_path not found or not a UCurveTable: %s"), *CurveTablePath));
        }
        return nullptr;
    }

    // A curve setter's curve_table_* and row_name_* fields, read back from Readback (null when the
    // magnitude's sub-struct is unreachable), and its set flag: both round-tripped.
    void GASAuthoring_AddCurveRowFields(const TSharedPtr<FJsonObject>& Result, const UCurveTable* CurveTable,
                                        const FString& RowName, const FCurveTableRowHandle* Readback, bool bRowExists)
    {
        FString CurveTableReadback;
        FString RowNameReadback;
        if (Readback)
        {
            if (const UCurveTable* RBTable = Readback->CurveTable.Get())
            {
                CurveTableReadback = RBTable->GetPathName();
            }
            RowNameReadback = Readback->RowName.ToString();
        }
        const bool bCurveMatches = !CurveTableReadback.IsEmpty() && CurveTableReadback == CurveTable->GetPathName();
        const bool bRowMatches = RowNameReadback == RowName;

        Result->SetStringField(TEXT("curve_table_path"), CurveTable->GetPathName());
        Result->SetStringField(TEXT("curve_table_readback"), CurveTableReadback);
        Result->SetBoolField(TEXT("curve_table_matches"), bCurveMatches);
        Result->SetStringField(TEXT("row_name"), RowName);
        Result->SetStringField(TEXT("row_name_readback"), RowNameReadback);
        Result->SetBoolField(TEXT("row_name_matches"), bRowMatches);
        Result->SetBoolField(TEXT("row_exists"), bRowExists);
        Result->SetBoolField(TEXT("set"), bCurveMatches && bRowMatches);
    }

    // Resolve a gameplay attribute by bare name (e.g. "Mana") or "Set.Attribute"
    // form, searching across all loaded UAttributeSet subclasses. Works without an
    // ASC — used for authoring a GE modifier's target attribute. Returns an invalid
    // attribute if not found.
    //
    // NOTE: We iterate UClass objects directly rather than calling
    // FGameplayAttribute::GetAllAttributeProperties(), because that helper
    // deliberately excludes UAbilitySystemTestAttributeSet and Blueprint-generated
    // sets (verified in AttributeSet.cpp). Iterating directly lets us resolve
    // attributes on any native attribute set, including the engine-provided test set.
    FGameplayAttribute ResolveAttributeByName(const FString& AttributeName)
    {
        if (AttributeName.IsEmpty())
        {
            return FGameplayAttribute();
        }

        for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
        {
            UClass* Class = *ClassIt;
            if (!Class || !Class->IsChildOf(UAttributeSet::StaticClass()) || Class == UAttributeSet::StaticClass())
            {
                continue;
            }

            // Include attributes declared directly on this set (ExcludeSuper avoids
            // duplicate matches; base-class attributes are found when iterating the
            // base class itself).
            for (TFieldIterator<FProperty> PropertyIt(Class, EFieldIteratorFlags::ExcludeSuper); PropertyIt; ++PropertyIt)
            {
                FProperty* Prop = *PropertyIt;
                if (!Prop || !FGameplayAttribute::IsSupportedProperty(Prop))
                {
                    continue;
                }
                const FString PropName = Prop->GetName();
                const FString Qualified = FString::Printf(TEXT("%s.%s"), *Class->GetName(), *PropName);
                if (PropName.Equals(AttributeName, ESearchCase::IgnoreCase)
                    || Qualified.Equals(AttributeName, ESearchCase::IgnoreCase))
                {
                    return FGameplayAttribute(Prop);
                }
            }
        }
        return FGameplayAttribute();
    }

    // Read the GE CDO's protected GEComponents array via reflection (the property is
    // EditDefaultsOnly/Instanced TArray<TObjectPtr<UGameplayEffectComponent>>). Returns
    // the live elements without needing friend/public access. Verified property name
    // "GEComponents" in GameplayEffect.h.
    void ReadGEComponents(UGameplayEffect* GECDO, TArray<UObject*>& OutComponents)
    {
        OutComponents.Reset();
        if (!GECDO)
        {
            return;
        }
        FArrayProperty* ArrayProp = FindFProperty<FArrayProperty>(UGameplayEffect::StaticClass(), TEXT("GEComponents"));
        if (!ArrayProp)
        {
            return;
        }
        FObjectPropertyBase* InnerObjProp = CastField<FObjectPropertyBase>(ArrayProp->Inner);
        if (!InnerObjProp)
        {
            return;
        }
        const void* ArrayValuePtr = ArrayProp->ContainerPtrToValuePtr<void>(GECDO);
        FScriptArrayHelper Helper(ArrayProp, ArrayValuePtr);
        for (int32 Index = 0; Index < Helper.Num(); ++Index)
        {
            const void* ElemPtr = Helper.GetRawPtr(Index);
            UObject* Obj = InnerObjProp->GetObjectPropertyValue(ElemPtr);
            OutComponents.Add(Obj);
        }
    }

    // Map a friendly component_type string to its concrete UClass + invoke the
    // header-inline UGameplayEffect::AddComponent<T>() template (the only public
    // mutator of the protected GEComponents array). Returns the added component (or
    // nullptr + OutError for an unknown type).
    UGameplayEffectComponent* AddGEComponentByType(UGameplayEffect* GECDO, const FString& ComponentType,
                                                   UClass*& OutClass, FString& OutError)
    {
        OutClass = nullptr;
        if (!GECDO)
        {
            OutError = TEXT("Null GameplayEffect CDO");
            return nullptr;
        }

        if (ComponentType.Equals(TEXT("AssetTags"), ESearchCase::IgnoreCase))
        {
            UAssetTagsGameplayEffectComponent& C = GECDO->AddComponent<UAssetTagsGameplayEffectComponent>();
            OutClass = UAssetTagsGameplayEffectComponent::StaticClass();
            return &C;
        }
        if (ComponentType.Equals(TEXT("TargetTags"), ESearchCase::IgnoreCase))
        {
            UTargetTagsGameplayEffectComponent& C = GECDO->AddComponent<UTargetTagsGameplayEffectComponent>();
            OutClass = UTargetTagsGameplayEffectComponent::StaticClass();
            return &C;
        }
        if (ComponentType.Equals(TEXT("AdditionalEffects"), ESearchCase::IgnoreCase))
        {
            UAdditionalEffectsGameplayEffectComponent& C = GECDO->AddComponent<UAdditionalEffectsGameplayEffectComponent>();
            OutClass = UAdditionalEffectsGameplayEffectComponent::StaticClass();
            return &C;
        }
        if (ComponentType.Equals(TEXT("ChanceToApply"), ESearchCase::IgnoreCase))
        {
            UChanceToApplyGameplayEffectComponent& C = GECDO->AddComponent<UChanceToApplyGameplayEffectComponent>();
            OutClass = UChanceToApplyGameplayEffectComponent::StaticClass();
            return &C;
        }
        if (ComponentType.Equals(TEXT("Immunity"), ESearchCase::IgnoreCase))
        {
            UImmunityGameplayEffectComponent& C = GECDO->AddComponent<UImmunityGameplayEffectComponent>();
            OutClass = UImmunityGameplayEffectComponent::StaticClass();
            return &C;
        }
        // The 6 remaining UGameplayEffectComponent subclasses. RemoveOther
        // carries a TArray<FGameplayEffectQuery>; the TYPE is addable, full query
        // authoring is deferred (honest note in the handler), so we only add the empty
        // component here (like the 5 above).
        if (ComponentType.Equals(TEXT("TargetTagRequirements"), ESearchCase::IgnoreCase))
        {
            UTargetTagRequirementsGameplayEffectComponent& C = GECDO->AddComponent<UTargetTagRequirementsGameplayEffectComponent>();
            OutClass = UTargetTagRequirementsGameplayEffectComponent::StaticClass();
            return &C;
        }
        if (ComponentType.Equals(TEXT("RemoveOther"), ESearchCase::IgnoreCase))
        {
            URemoveOtherGameplayEffectComponent& C = GECDO->AddComponent<URemoveOtherGameplayEffectComponent>();
            OutClass = URemoveOtherGameplayEffectComponent::StaticClass();
            return &C;
        }
        if (ComponentType.Equals(TEXT("CustomCanApply"), ESearchCase::IgnoreCase))
        {
            UCustomCanApplyGameplayEffectComponent& C = GECDO->AddComponent<UCustomCanApplyGameplayEffectComponent>();
            OutClass = UCustomCanApplyGameplayEffectComponent::StaticClass();
            return &C;
        }
        if (ComponentType.Equals(TEXT("BlockAbilityTags"), ESearchCase::IgnoreCase))
        {
            UBlockAbilityTagsGameplayEffectComponent& C = GECDO->AddComponent<UBlockAbilityTagsGameplayEffectComponent>();
            OutClass = UBlockAbilityTagsGameplayEffectComponent::StaticClass();
            return &C;
        }
        if (ComponentType.Equals(TEXT("Abilities"), ESearchCase::IgnoreCase))
        {
            UAbilitiesGameplayEffectComponent& C = GECDO->AddComponent<UAbilitiesGameplayEffectComponent>();
            OutClass = UAbilitiesGameplayEffectComponent::StaticClass();
            return &C;
        }
        if (ComponentType.Equals(TEXT("CancelAbilityTags"), ESearchCase::IgnoreCase))
        {
            UCancelAbilityTagsGameplayEffectComponent& C = GECDO->AddComponent<UCancelAbilityTagsGameplayEffectComponent>();
            OutClass = UCancelAbilityTagsGameplayEffectComponent::StaticClass();
            return &C;
        }

        OutError = FString::Printf(
            TEXT("Unknown component_type '%s' (expected AssetTags|TargetTags|AdditionalEffects|ChanceToApply|Immunity|")
            TEXT("TargetTagRequirements|RemoveOther|CustomCanApply|BlockAbilityTags|Abilities|CancelAbilityTags)"),
            *ComponentType);
        return nullptr;
    }

    // ========================================================================
    // GameplayEffectComponent CONFIG SETTERS. add_gameplay_effect_component
    // only ADDS an empty component; these setters CONFIGURE it. Idempotency: re-using a component is via
    // UGameplayEffect::FindOrAddComponent<T>() (header-inline at GameplayEffect.h,
    // cross-module safe) so re-running a setter mutates the existing component
    // rather than stacking a duplicate.
    // ========================================================================

    // Parse a JSON array of gameplay-tag strings into a registered FGameplayTagContainer.
    // Resolved tags are added; unknown tag strings are collected for an honest readback.
    // ErrorIfNotFound=false: unknown tags are reported, not logged.
    void GASAuthoring_ParseTags(const TArray<TSharedPtr<FJsonValue>>& In, FGameplayTagContainer& OutContainer,
                                TArray<TSharedPtr<FJsonValue>>& OutResolved, TArray<TSharedPtr<FJsonValue>>& OutUnknown)
    {
        UGameplayTagsManager& TagsManager = UGameplayTagsManager::Get();
        for (const TSharedPtr<FJsonValue>& Value : In)
        {
            FString TagStr;
            if (!Value.IsValid() || !Value->TryGetString(TagStr) || TagStr.IsEmpty())
            {
                continue;
            }
            const FGameplayTag Tag = TagsManager.RequestGameplayTag(FName(*TagStr), /*ErrorIfNotFound=*/false);
            if (Tag.IsValid())
            {
                OutContainer.AddTag(Tag);
                OutResolved.Add(MakeShared<FJsonValueString>(Tag.ToString()));
            }
            else
            {
                OutUnknown.Add(MakeShared<FJsonValueString>(TagStr));
            }
        }
    }

    // Persist a CDO edit: mark the Blueprint modified and dirty and save its package, without recompiling
    // (values on the CDO serialize with the package). Modify() on the CDO is the caller's responsibility
    // (done before mutating). A save that fails undoes Edit, the snapshot taken before the edit, so the error
    // reply leaves the asset as it was; a saved edit is kept. Null when saved, else the error reply.
    TSharedPtr<FJsonObject> GASAuthoring_SaveBlueprint(UBlueprint* Blueprint, FGASAuthoringCDOEdit& Edit)
    {
        if (!Blueprint)
        {
            return mcp_common::MakeErrorReply(TEXT("Null Blueprint"));
        }
        Blueprint->Modify();
        Blueprint->MarkPackageDirty();
        FString SaveError;
        if (!mcp_common::SaveAssetPackage(Blueprint->GetOutermost(), Blueprint, SaveError))
        {
            Edit.Undo();
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("%s The edit was undone, so the asset is as it was before the call."), *SaveError));
        }
        Edit.Keep();
        return nullptr;
    }

    // The save of an AttributeSet Blueprint whose member variables an edit added and compiled: true when its
    // package reached disk, else false with OutSaveError.
    bool GASAuthoring_SaveAttributeSet(UBlueprint* Blueprint, FString& OutSaveError)
    {
        Blueprint->Modify();
        Blueprint->MarkPackageDirty();
        return mcp_common::SaveAssetPackage(Blueprint->GetOutermost(), Blueprint, OutSaveError);
    }

    // Adds AttributeName to Blueprint as an FGameplayAttributeData member variable whose BaseValue and
    // CurrentValue default to DefaultValue. False when the Blueprint or its parents already have a variable
    // of that name (AddMemberVariable refuses it). The compiler imports a struct default as struct text,
    // so the default is the struct exported as text: a bare number does not import and leaves 0.
    bool GASAuthoring_AddAttributeVariable(UBlueprint* Blueprint, const FString& AttributeName, double DefaultValue)
    {
        FEdGraphPinType PinType;
        PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
        PinType.PinSubCategoryObject = FGameplayAttributeData::StaticStruct();
        const FGameplayAttributeData Default(static_cast<float>(DefaultValue));
        FString DefaultText;
        FGameplayAttributeData::StaticStruct()->ExportText(DefaultText, &Default, /*Defaults*/ nullptr,
            /*OwnerObject*/ nullptr, PPF_SerializedAsImportText, /*ExportRootScope*/ nullptr);
        return FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*AttributeName), PinType, DefaultText);
    }

    // Whether GASAuthoring_AddAttributeVariable would add AttributeName to Blueprint, decided without touching it:
    // AddMemberVariable's own test, which refuses NAME_None and a name the class or its parents already have as a
    // variable. A call that adds nothing writes nothing, so it is neither refused up front nor undone.
    bool GASAuthoring_CanAddAttributeVariable(const UBlueprint* Blueprint, const FString& AttributeName)
    {
        const FName VarName(*AttributeName);
        if (VarName == NAME_None)
        {
            return false;
        }
        TSet<FName> CurrentVars;
        FBlueprintEditorUtils::GetClassVariableList(Blueprint, CurrentVars);
        return !CurrentVars.Contains(VarName);
    }

    // The undo of a GASAuthoring_AddAttributeVariable whose save failed: takes AttributeName out of Blueprint as the
    // editor deletes a variable (Modify, then FBlueprintEditorUtils::RemoveMemberVariable) and recompiles it, so its
    // generated class and CDO lose the member too while the compile carries the other members' CDO values over.
    // Compiling is safe because the add refused a running PIE session up front. True when the member is gone from
    // the Blueprint and its class; only then is the package's dirty flag put back to bPackageWasDirty.
    bool GASAuthoring_RemoveAddedAttribute(UBlueprint* Blueprint, const FString& AttributeName, bool bPackageWasDirty)
    {
        const FName VarName(*AttributeName);
        Blueprint->Modify();
        FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VarName);
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
        UClass* GeneratedClass = Blueprint->GeneratedClass;
        const bool bRemoved = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName) == INDEX_NONE
            && !(GeneratedClass && FindFProperty<FProperty>(GeneratedClass, VarName));
        if (bRemoved)
        {
            Blueprint->GetOutermost()->SetDirtyFlag(bPackageWasDirty);
            // The add and its removal cancel out: the call's undo step would undo nothing.
            mcp_common::DiscardUndoStepOfThisCall();
        }
        return bRemoved;
    }

    // The BaseValue of the FGameplayAttributeData member AttributeName on the CDO of Blueprint's generated
    // class, into OutBaseValue. False when the class has no member of that name or it is of another type.
    bool GASAuthoring_ReadAttributeBaseValue(const UBlueprint* Blueprint, const FString& AttributeName, float& OutBaseValue)
    {
        UClass* GeneratedClass = Blueprint->GeneratedClass;
        const FStructProperty* Prop = GeneratedClass
            ? FindFProperty<FStructProperty>(GeneratedClass, FName(*AttributeName))
            : nullptr;
        if (!Prop || Prop->Struct != FGameplayAttributeData::StaticStruct())
        {
            return false;
        }
        OutBaseValue = Prop->ContainerPtrToValuePtr<FGameplayAttributeData>(GeneratedClass->GetDefaultObject())->GetBaseValue();
        return true;
    }

    // Whether a BaseValue read back from the CDO is the default_value that was asked for. The default
    // travels as struct text with six decimals, so the comparison allows for that rounding.
    bool GASAuthoring_DefaultApplied(float BaseValue, double Requested)
    {
        return FMath::IsNearlyEqual(static_cast<double>(BaseValue), Requested, 1e-5 * FMath::Max(1.0, FMath::Abs(Requested)));
    }

    // Read an FGameplayAbilitySpecConfig's public fields into JSON via reflection. The struct
    // is USTRUCT() with no _API, so we resolve its UScriptStruct by name (NativeFirst) — the
    // link-safe by-name pattern (same as GetMagnitudeSubStructPtr). Returns the Ability class
    // name, level, input ID and removal-policy index.
    TSharedPtr<FJsonObject> AbilitySpecConfigToJson(const void* ConfigPtr, UScriptStruct* ConfigStruct)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        if (!ConfigPtr || !ConfigStruct)
        {
            return Entry;
        }
        if (FClassProperty* AbilityProp = FindFProperty<FClassProperty>(ConfigStruct, TEXT("Ability")))
        {
            UObject* AbilityVal = AbilityProp->GetObjectPropertyValue(AbilityProp->ContainerPtrToValuePtr<void>(ConfigPtr));
            UClass* AbilityClass = Cast<UClass>(AbilityVal);
            Entry->SetStringField(TEXT("ability_class"), AbilityClass ? AbilityClass->GetName() : FString());
            Entry->SetStringField(TEXT("ability_path"), AbilityClass ? AbilityClass->GetPathName() : FString());
        }
        if (FIntProperty* InputProp = FindFProperty<FIntProperty>(ConfigStruct, TEXT("InputID")))
        {
            Entry->SetNumberField(TEXT("input_id"), InputProp->GetPropertyValue_InContainer(ConfigPtr));
        }
        return Entry;
    }

    // Resolve a GameplayEffect Blueprint path to its UGameplayEffect-derived generated class.
    UClass* LoadGameplayEffectClass(const FString& GEPath, FString& OutError)
    {
        UBlueprint* GEBP = LoadObject<UBlueprint>(nullptr, *GEPath);
        if (!GEBP || !GEBP->GeneratedClass)
        {
            OutError = FString::Printf(TEXT("GameplayEffect Blueprint not found (or no GeneratedClass): %s"), *GEPath);
            return nullptr;
        }
        if (!GEBP->GeneratedClass->IsChildOf(UGameplayEffect::StaticClass()))
        {
            OutError = FString::Printf(TEXT("Blueprint is not a UGameplayEffect: %s"), *GEPath);
            return nullptr;
        }
        return GEBP->GeneratedClass;
    }

    // Shared impl for set_ability_cost / set_ability_cooldown: assign a GameplayEffect class to a
    // PROTECTED TSubclassOf<UGameplayEffect> UPROPERTY on a UGameplayAbility CDO via reflection
    // (FClassProperty; GameplayAbility.h are under `protected:` -> no friend access), with read-back.
    // PropName = CostGameplayEffectClass | CooldownGameplayEffectClass.
    TSharedPtr<FJsonObject> SetAbilityGameplayEffectClass(
        const TSharedPtr<FJsonObject>& Params, const TCHAR* PropName,
        const TCHAR* GEParamName, const TCHAR* RespClassKey)
    {
        FString AbilityPath;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ability_blueprint_path"), AbilityPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        FString GEPath;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, GEParamName, GEPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

        UBlueprint* AbilityBP = LoadObject<UBlueprint>(nullptr, *AbilityPath);
        if (!AbilityBP || !AbilityBP->GeneratedClass)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("GameplayAbility Blueprint not found (or no GeneratedClass): %s"), *AbilityPath));
        }
        UGameplayAbility* AbilityCDO = Cast<UGameplayAbility>(AbilityBP->GeneratedClass->GetDefaultObject());
        if (!AbilityCDO)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Blueprint is not a UGameplayAbility: %s"), *AbilityPath));
        }

        FString GEErr;
        UClass* GEClass = LoadGameplayEffectClass(GEPath, GEErr);
        if (!GEClass)
        {
            return mcp_common::MakeErrorReply(GEErr);
        }

        // TSubclassOf<UGameplayEffect> reflects as FClassProperty (MetaClass = UGameplayEffect). The field is
        // protected, but reflection ignores C++ access. SetObjectPropertyValue stores the UClass* directly.
        FClassProperty* ClassProp = FindFProperty<FClassProperty>(UGameplayAbility::StaticClass(), PropName);
        if (!ClassProp)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Reflection: property %s not found on UGameplayAbility"), PropName));
        }
        if (TSharedPtr<FJsonObject> Refused = GASAuthoring_RefuseUnsavableEdit(AbilityBP))
        {
            return Refused;
        }

        FGASAuthoringCDOEdit Edit(AbilityCDO);
        AbilityCDO->Modify();
        ClassProp->SetObjectPropertyValue(ClassProp->ContainerPtrToValuePtr<void>(AbilityCDO), GEClass);

        // Native UPROPERTY on the parent class -> values live on the CDO and serialize with the package;
        // mark dirty + save, no recompile (same rationale as the GE CDO setters).
        if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(AbilityBP, Edit))
        {
            return SaveFailed;
        }

        UObject* RB = ClassProp->GetObjectPropertyValue(ClassProp->ContainerPtrToValuePtr<void>(AbilityCDO));
        UClass* RBClass = Cast<UClass>(RB);
        const bool bMatches = (RBClass == GEClass);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("ability_blueprint_path"), AbilityPath);
        Result->SetStringField(GEParamName, GEPath);
        Result->SetStringField(RespClassKey, GEClass->GetName());
        Result->SetBoolField(FString::Printf(TEXT("%s_matches"), RespClassKey), bMatches);
        Result->SetBoolField(TEXT("set"), bMatches);
        return mcp_common::MakeOkReply(Result);
    }

    // Resolve a UGameplayModMagnitudeCalculation subclass from a /Game BP asset path
    // (-> GeneratedClass) or a bare class name (reflection over loaded classes, native first).
    UClass* ResolveModMagnitudeCalcClass(const FString& In)
    {
        return FUEAgentGraphUtils::ResolveClass(In);
    }
}

// Blueprint creation

// MCP:create_gameplay_ability_blueprint - Create a Blueprint parented to UGameplayAbility at package_path/name, compile and save it; returns asset_path, generated_class and is_gameplay_ability. A GameplayAbility Blueprint already at that path is returned untouched (created false, already_existed true); a Blueprint of another class there is an error. Refused during PIE.
//   @param name - Asset name without a path, e.g. "GA_Fireball".
//   @param package_path - Content folder for the new asset, e.g. "/Game/GAS/Abilities"; defaults to /Game/Blueprints.
//   @returns { name: string, asset_path: string, parent_class: string, generated_class: string, is_gameplay_ability: bool, already_existed: bool, created: bool }
TSharedPtr<FJsonObject> HandleCreateGameplayAbilityBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FGASAuthoringCreatedBlueprint Created;
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_CreateBlueprint(Params, UGameplayAbility::StaticClass(), Created))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Result = GASAuthoring_CreatedBlueprintFields(Created, TEXT("GameplayAbility"), TEXT("is_gameplay_ability"));
    Result->SetBoolField(TEXT("already_existed"), Created.bAlreadyExisted);
    Result->SetBoolField(TEXT("created"), !Created.bAlreadyExisted);
    return mcp_common::MakeOkReply(Result);
}

// MCP:create_gameplay_effect_blueprint - Create a Blueprint parented to UGameplayEffect at package_path/name, compile and save it; returns asset_path, generated_class and is_gameplay_effect. A GameplayEffect Blueprint already at that path is returned untouched (created false, already_existed true); a Blueprint of another class there is an error. Refused during PIE.
//   @param name - Asset name without a path, e.g. "GE_Damage".
//   @param package_path - Content folder for the new asset, e.g. "/Game/GAS/Effects"; defaults to /Game/Blueprints.
//   @returns { name: string, asset_path: string, parent_class: string, generated_class: string, is_gameplay_effect: bool, already_existed: bool, created: bool }
TSharedPtr<FJsonObject> HandleCreateGameplayEffectBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FGASAuthoringCreatedBlueprint Created;
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_CreateBlueprint(Params, UGameplayEffect::StaticClass(), Created))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Result = GASAuthoring_CreatedBlueprintFields(Created, TEXT("GameplayEffect"), TEXT("is_gameplay_effect"));
    Result->SetBoolField(TEXT("already_existed"), Created.bAlreadyExisted);
    Result->SetBoolField(TEXT("created"), !Created.bAlreadyExisted);
    return mcp_common::MakeOkReply(Result);
}

// MCP:create_attribute_set_blueprint - Create an empty Blueprint parented to UAttributeSet at package_path/name, compile and save it; returns asset_path and is_attribute_set. An AttributeSet Blueprint already there is returned untouched (created false, already_existed true); a Blueprint of another class there is an error. To add attributes use add_attribute_to_set, or create_attribute_set_asset in one call. Refused during PIE.
//   @param name - Asset name without a path, e.g. "AS_Character".
//   @param package_path - Content folder for the new asset, e.g. "/Game/GAS/Attributes"; defaults to /Game/Blueprints.
//   @returns { name: string, asset_path: string, parent_class: string, generated_class: string, is_attribute_set: bool, already_existed: bool, created: bool }
TSharedPtr<FJsonObject> HandleCreateAttributeSetBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FGASAuthoringCreatedBlueprint Created;
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_CreateBlueprint(Params, UAttributeSet::StaticClass(), Created))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Result = GASAuthoring_CreatedBlueprintFields(Created, TEXT("AttributeSet"), TEXT("is_attribute_set"));
    Result->SetBoolField(TEXT("already_existed"), Created.bAlreadyExisted);
    Result->SetBoolField(TEXT("created"), !Created.bAlreadyExisted);
    return mcp_common::MakeOkReply(Result);
}

// AttributeSet authoring: create an AttributeSet Blueprint with FGameplayAttributeData member variables, or
// append one such member to an existing AttributeSet Blueprint. FBlueprintEditorUtils::AddMemberVariable is
// the canonical Blueprint variable creator (BlueprintEditorUtils.h).

// MCP:create_attribute_set_asset - Create (or reuse) an AttributeSet Blueprint at package_path/name like create_attribute_set_blueprint (a Blueprint of another class there is an error, and is left untouched), add one FGameplayAttributeData variable per attributes entry, then compile and save it. A name the class already has is skipped and listed in skipped_attribute_names; success is false when an added attribute is missing from the compiled class or did not get its default_value. A running PIE session, a read-only root or a .uasset read-only on disk fails before anything is created or added; a call that adds nothing to an existing set is not refused and saves nothing (saved false). A failed save makes success false, with saved false and save_error, and the added attributes stay in memory only.
//   @param name - Asset name without a path, e.g. "AS_Character".
//   @param package_path - Content folder for the new asset, e.g. "/Game/GAS/Attributes"; defaults to /Game/Blueprints.
//   @param attributes - Array of {"name": "Health", "default_value": 100} objects; default_value (default 0) becomes the BaseValue and CurrentValue of the compiled default. Entries without a name are ignored.
//   @returns { name: string, asset_path: string, parent_class: string, generated_class: string, is_attribute_set: bool, already_existed: bool, attributes_added: int, added_attribute_names: [string], skipped_attribute_names: [string], failed_attribute_names: [string], attributes_applied: bool, created: bool, saved: bool, save_error?: string }
TSharedPtr<FJsonObject> HandleCreateAttributeSetAsset(const TSharedPtr<FJsonObject>& Params)
{
    FGASAuthoringCreatedBlueprint Created;
    if (TSharedPtr<FJsonObject> Missing = GASAuthoring_ReadCreateTarget(Params, Created))
    {
        return Missing;
    }

    // A new asset is saved as it is created, so what keeps a save from happening refuses the call before anything
    // is created (the path is validated first, as CreateOrLoadBlueprint does, by the same existence test it uses).
    // An existing one is refused further down, and only when the call adds to it.
    const FString AssetPath = GASAuthoring_CreateTargetPath(Created);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(AssetPath))
    {
        return Invalid;
    }
    if (!UEditorAssetLibrary::DoesAssetExist(AssetPath))
    {
        if (TSharedPtr<FJsonObject> Refused = GASAuthoring_RefuseUnsavablePackage(AssetPath))
        {
            return Refused;
        }
    }

    if (TSharedPtr<FJsonObject> Error = GASAuthoring_CreateOrLoadTarget(UAttributeSet::StaticClass(), Created))
    {
        return Error;
    }
    UBlueprint* Blueprint = Created.Blueprint;

    // Optional: list of {name, default_value} pairs, each added as an FGameplayAttributeData member variable.
    const TArray<TSharedPtr<FJsonValue>>* AttrArray = nullptr;
    Params->TryGetArrayField(TEXT("attributes"), AttrArray);

    // The named entries, with the default each one asks for, read before anything is added.
    TArray<TPair<FString, double>> Requested;
    if (AttrArray && Blueprint->GeneratedClass)
    {
        for (const TSharedPtr<FJsonValue>& AttrVal : *AttrArray)
        {
            const TSharedPtr<FJsonObject>* AttrObj = nullptr;
            if (!AttrVal.IsValid() || !AttrVal->TryGetObject(AttrObj) || !AttrObj || !(*AttrObj).IsValid())
            {
                continue;
            }
            const TSharedPtr<FJsonObject>& Attr = *AttrObj;
            FString AttrName;
            if (!Attr->TryGetStringField(TEXT("name"), AttrName) || AttrName.IsEmpty())
            {
                continue;
            }
            double DefaultValue = 0.0;
            Attr->TryGetNumberField(TEXT("default_value"), DefaultValue);
            Requested.Emplace(AttrName, DefaultValue);
        }
    }

    // An existing AttributeSet the call adds to is refused before the first add when that add could not be saved.
    const bool bAddsAny = Requested.ContainsByPredicate([Blueprint](const TPair<FString, double>& Attribute)
    {
        return GASAuthoring_CanAddAttributeVariable(Blueprint, Attribute.Key);
    });
    if (bAddsAny && Created.bAlreadyExisted)
    {
        if (TSharedPtr<FJsonObject> Refused = GASAuthoring_RefuseUnsavableEdit(Blueprint))
        {
            return Refused;
        }
    }

    // The names AddMemberVariable accepted, with the default each one asked for; and the names it refused
    // because the class already has a variable of that name (it refuses them without touching the Blueprint).
    TArray<TPair<FString, double>> Accepted;
    TArray<TSharedPtr<FJsonValue>> SkippedNames;
    for (const TPair<FString, double>& Attribute : Requested)
    {
        if (GASAuthoring_AddAttributeVariable(Blueprint, Attribute.Key, Attribute.Value))
        {
            Accepted.Add(Attribute);
        }
        else
        {
            SkippedNames.Add(MakeShared<FJsonValueString>(Attribute.Key));
        }
    }

    // The new members reach the generated class, and their defaults the CDO, only on a compile. A new asset was
    // already saved by its creation; one this call adds nothing to is not saved again.
    bool bSaved = !Created.bAlreadyExisted;
    FString SaveError;
    if (Accepted.Num() > 0)
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
        bSaved = GASAuthoring_SaveAttributeSet(Blueprint, SaveError);
    }

    // Read each added attribute back from the compiled class and its CDO.
    TArray<TSharedPtr<FJsonValue>> AddedNames;
    TArray<TSharedPtr<FJsonValue>> FailedNames;
    for (const TPair<FString, double>& Attribute : Accepted)
    {
        float BaseValue = 0.f;
        const bool bApplied = GASAuthoring_ReadAttributeBaseValue(Blueprint, Attribute.Key, BaseValue)
            && GASAuthoring_DefaultApplied(BaseValue, Attribute.Value);
        (bApplied ? AddedNames : FailedNames).Add(MakeShared<FJsonValueString>(Attribute.Key));
    }

    TSharedPtr<FJsonObject> Result = GASAuthoring_CreatedBlueprintFields(Created, TEXT("AttributeSet"), TEXT("is_attribute_set"));
    Result->SetBoolField(TEXT("already_existed"), Created.bAlreadyExisted);
    Result->SetNumberField(TEXT("attributes_added"), AddedNames.Num());
    Result->SetArrayField(TEXT("added_attribute_names"), AddedNames);
    Result->SetArrayField(TEXT("skipped_attribute_names"), SkippedNames);
    Result->SetArrayField(TEXT("failed_attribute_names"), FailedNames);
    const bool bApplied = FailedNames.Num() == 0;
    Result->SetBoolField(TEXT("attributes_applied"), bApplied);
    Result->SetBoolField(TEXT("created"), !Created.bAlreadyExisted);
    Result->SetBoolField(TEXT("saved"), bSaved);
    if (Accepted.Num() > 0 && !bSaved)
    {
        // As with the other create tools: the attributes stay on the Blueprint in memory only, its package dirty.
        return mcp_common::MakeSavedReply(Result, /*bSaved*/ false, TEXT("create_attribute_set_asset"), SaveError);
    }
    return mcp_common::MakeOutcomeReply(Result, bApplied,
        bApplied ? FString() : mcp_common::MeasuredOutcomeError(TEXT("create_attribute_set_asset"), TEXT("attributes_applied")));
}

// MCP:add_attribute_to_set - Add one FGameplayAttributeData member variable to an AttributeSet Blueprint, with default_value as its BaseValue and CurrentValue, then compile and save it. A name that is already an attribute of the class is left as it is (added false, already_existed true, nothing saved); a name taken by a variable of another type fails. attribute_present and base_value are read from the compiled class. An add is refused before any edit when a running PIE session, a read-only root or a .uasset read-only on disk would keep it from being saved, and a failed save undoes it, taking the variable out again and recompiling the Blueprint.
//   @param attribute_set_path - Full asset path of the AttributeSet Blueprint
//   @param attribute_name - Name of the new attribute member variable
//   @param default_value - Initial BaseValue and CurrentValue of the attribute, default 0; not applied to an attribute that already exists.
//   @returns { attribute_set_path: string, attribute_name: string, default_value: number, added: bool, already_existed: bool, attribute_present: bool, base_value?: number, default_applied?: bool, saved: bool }
TSharedPtr<FJsonObject> HandleAddAttributeToSet(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("attribute_set_path"), BlueprintPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString AttributeName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("attribute_name"), AttributeName)) { return Missing; }
    double DefaultValue = 0.0;
    Params->TryGetNumberField(TEXT("default_value"), DefaultValue);

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("AttributeSet Blueprint not found: %s"), *BlueprintPath));
    }
    if (!Blueprint->GeneratedClass || !Blueprint->GeneratedClass->IsChildOf(UAttributeSet::StaticClass()))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Blueprint is not a UAttributeSet: %s"), *BlueprintPath));
    }

    // Only an add changes the Blueprint, so only an add is refused when it could not be saved; a name the class
    // already has is answered without touching it.
    const bool bAddable = GASAuthoring_CanAddAttributeVariable(Blueprint, AttributeName);
    if (bAddable)
    {
        if (TSharedPtr<FJsonObject> Refused = GASAuthoring_RefuseUnsavableEdit(Blueprint))
        {
            return Refused;
        }
    }

    const bool bPackageWasDirty = Blueprint->GetOutermost()->IsDirty();
    const bool bAdded = bAddable && GASAuthoring_AddAttributeVariable(Blueprint, AttributeName, DefaultValue);
    if (bAdded)
    {
        // The new member reaches the generated class, and its default the CDO, only on a compile.
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
        FString SaveError;
        if (!GASAuthoring_SaveAttributeSet(Blueprint, SaveError))
        {
            // A save that fails for a reason the refusal could not see (the file held open, a full disk) takes the
            // attribute out again, so the error leaves the asset as it was.
            if (GASAuthoring_RemoveAddedAttribute(Blueprint, AttributeName, bPackageWasDirty))
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("%s The edit was undone: the attribute was removed again and the Blueprint recompiled, so the ")
                    TEXT("asset is as it was before the call."), *SaveError));
            }
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("%s The attribute could not be removed again, so it stays on the Blueprint in memory only, its ")
                TEXT("package dirty."), *SaveError));
        }
    }

    float BaseValue = 0.f;
    const bool bPresent = GASAuthoring_ReadAttributeBaseValue(Blueprint, AttributeName, BaseValue);
    if (!bAdded && !bPresent)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Cannot add '%s' to %s: the name is not a valid variable name or is taken by a variable that is not an FGameplayAttributeData"),
            *AttributeName, *BlueprintPath));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("attribute_set_path"), BlueprintPath);
    Result->SetStringField(TEXT("attribute_name"), AttributeName);
    Result->SetNumberField(TEXT("default_value"), DefaultValue);
    Result->SetBoolField(TEXT("added"), bAdded);
    Result->SetBoolField(TEXT("already_existed"), !bAdded);
    Result->SetBoolField(TEXT("attribute_present"), bPresent);
    if (bPresent)
    {
        Result->SetNumberField(TEXT("base_value"), BaseValue);
    }
    // An attribute added by this call must be on the compiled class with the requested default.
    const bool bDefaultApplied = bPresent && bAdded && GASAuthoring_DefaultApplied(BaseValue, DefaultValue);
    if (bAdded)
    {
        Result->SetBoolField(TEXT("default_applied"), bDefaultApplied);
    }
    Result->SetBoolField(TEXT("saved"), bAdded);
    const bool bOk = bAdded ? bDefaultApplied : bPresent;
    return mcp_common::MakeOutcomeReply(Result, bOk, bOk ? FString()
        : mcp_common::MeasuredOutcomeError(TEXT("add_attribute_to_set"), bPresent ? TEXT("default_applied") : TEXT("attribute_present")));
}

// GameplayEffect CDO authoring

// MCP:set_gameplay_effect_duration - Set DurationPolicy on the class default object of a GameplayEffect Blueprint and, for HasDuration, a constant DurationMagnitude of duration_seconds (Instant and Infinite leave the stored magnitude as it was), then save the asset without recompiling and without an undo entry. A bad policy, HasDuration without duration_seconds, a non-GE Blueprint, a running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit. duration_seconds and duration_readback_ok come back only for HasDuration. For a native GE asset use set_gameplay_effect_duration_policy; for periodic ticks, set_gameplay_effect_period.
//   @param ge_blueprint_path - Full asset path of the GE Blueprint
//   @param duration_policy - Instant, Infinite, or HasDuration
//   @param duration_seconds - Duration in seconds; required when duration_policy is HasDuration, ignored for Instant and Infinite
//   @returns { ge_blueprint_path: string, duration_policy: string, policy_matches: bool, duration_readback_ok: bool, duration_seconds: number, set: bool }
TSharedPtr<FJsonObject> HandleSetGameplayEffectDuration(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ge_blueprint_path"), BlueprintPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString DurationPolicyStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("duration_policy"), DurationPolicyStr, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // Map the policy string to the enum (GameplayEffect.h).
    EGameplayEffectDurationType Policy;
    if (DurationPolicyStr.Equals(TEXT("Instant"), ESearchCase::IgnoreCase))
    {
        Policy = EGameplayEffectDurationType::Instant;
    }
    else if (DurationPolicyStr.Equals(TEXT("Infinite"), ESearchCase::IgnoreCase))
    {
        Policy = EGameplayEffectDurationType::Infinite;
    }
    else if (DurationPolicyStr.Equals(TEXT("HasDuration"), ESearchCase::IgnoreCase))
    {
        Policy = EGameplayEffectDurationType::HasDuration;
    }
    else
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid duration_policy '%s' (expected Instant|Infinite|HasDuration)"), *DurationPolicyStr));
    }

    // duration_seconds is needed only for HasDuration, so it is optional in the schema and checked here, before
    // anything is written.
    const bool bHasDuration = Policy == EGameplayEffectDurationType::HasDuration;
    double DurationSeconds = 0.0;
    const bool bDurationGiven = Params->TryGetNumberField(TEXT("duration_seconds"), DurationSeconds);
    if (bHasDuration && !bDurationGiven)
    {
        return mcp_common::MakeErrorReply(
            TEXT("duration_seconds is required when duration_policy is HasDuration; nothing was changed"));
    }

    UBlueprint* Blueprint = nullptr;
    FString Error;
    UGameplayEffect* GECDO = LoadGameplayEffectCDO(BlueprintPath, Blueprint, Error);
    if (!GECDO)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    if (TSharedPtr<FJsonObject> Refused = GASAuthoring_RefuseUnsavableEdit(Blueprint))
    {
        return Refused;
    }

    FGASAuthoringCDOEdit Edit(GECDO);
    GECDO->Modify();

    GECDO->DurationPolicy = Policy;
    if (bHasDuration)
    {
        // FGameplayEffectModifierMagnitude has a public ctor from FScalableFloat, which
        // has a public ctor from float (ScalableFloat.h / GameplayEffect.h).
        GECDO->DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat((float)DurationSeconds));
    }

    // Persist the CDO edit. We intentionally do NOT recompile here: DurationPolicy/
    // DurationMagnitude are NATIVE UProperties on the UGameplayEffect parent class
    // (not Blueprint NewVariables), so their values live on the CDO and serialize with
    // the package directly. Recompiling risks re-initializing the CDO; marking dirty +
    // saving is sufficient and deterministic.
    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Blueprint, Edit))
    {
        return SaveFailed;
    }

    // Read back from the CDO (still holds our edit; no recompile occurred).
    UGameplayEffect* ReadCDO = GECDO;
    const EGameplayEffectDurationType ReadPolicy = ReadCDO ? ReadCDO->DurationPolicy : Policy;

    FString ReadPolicyStr;
    switch (ReadPolicy)
    {
        case EGameplayEffectDurationType::Instant:     ReadPolicyStr = TEXT("Instant");     break;
        case EGameplayEffectDurationType::Infinite:    ReadPolicyStr = TEXT("Infinite");    break;
        case EGameplayEffectDurationType::HasDuration: ReadPolicyStr = TEXT("HasDuration"); break;
        default:                                       ReadPolicyStr = TEXT("Unknown");     break;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("ge_blueprint_path"), BlueprintPath);
    Result->SetStringField(TEXT("duration_policy"), ReadPolicyStr);
    Result->SetBoolField(TEXT("policy_matches"), ReadPolicy == Policy);
    if (bHasDuration && ReadCDO && ReadPolicy == EGameplayEffectDurationType::HasDuration)
    {
        // Read the static magnitude back out for confirmation (GetStaticMagnitudeIfPossible).
        float ReadDuration = 0.f;
        const bool bGotMag = ReadCDO->DurationMagnitude.GetStaticMagnitudeIfPossible(1.0f, ReadDuration);
        Result->SetBoolField(TEXT("duration_readback_ok"), bGotMag);
        Result->SetNumberField(TEXT("duration_seconds"), ReadDuration);
    }
    Result->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeOkReply(Result);
}

// GameplayEffect period (DoT/HoT) + stacking policy (native UGameplayEffect UPROPERTYs).
// APIs (GameplayAbilities plugin):
//   UGameplayEffect::Period (FScalableFloat) / bExecutePeriodicEffectOnApplication (bool)  GameplayEffect.h
//   UGameplayEffect::StackingType (EGameplayEffectStackingType) / StackLimitCount (int32)   GameplayEffect.h
//   UGameplayEffect::StackDurationRefreshPolicy / StackPeriodResetPolicy                     GameplayEffect.h
//   FScalableFloat::GetValue() (UE_API)  ScalableFloat.h ; EGameplayEffectStackingType (None/AggregateBySource/AggregateByTarget)

// MCP:set_gameplay_effect_period - Set Period (seconds between periodic executions, 0 = not periodic) and bExecutePeriodicEffectOnApplication on the class default object of a GameplayEffect Blueprint, then save it without recompiling and without an undo entry. execute_on_application is always written, so leaving it out sets false. A non-GE Blueprint, a running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit. For damage or heal over time also give the effect a duration with set_gameplay_effect_duration; stacking is set_gameplay_effect_stacking.
//   @param ge_blueprint_path - Full asset path of the GE Blueprint.
//   @param period_seconds - Period in seconds between executions (0 = non-periodic).
//   @param execute_on_application - Optional: if true the effect also executes immediately on application (default false).
//   @returns { ge_blueprint_path, period_seconds, period_readback_ok, execute_on_application, set }
TSharedPtr<FJsonObject> HandleSetGameplayEffectPeriod(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ge_blueprint_path"), BlueprintPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    double PeriodSeconds = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("period_seconds"), PeriodSeconds)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    FString Error;
    UGameplayEffect* GECDO = LoadGameplayEffectCDO(BlueprintPath, Blueprint, Error);
    if (!GECDO)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    if (TSharedPtr<FJsonObject> Refused = GASAuthoring_RefuseUnsavableEdit(Blueprint))
    {
        return Refused;
    }

    FGASAuthoringCDOEdit Edit(GECDO);
    GECDO->Modify();
    GECDO->Period = FScalableFloat((float)PeriodSeconds);
    bool bExecOnApp = false;
    Params->TryGetBoolField(TEXT("execute_on_application"), bExecOnApp);
    GECDO->bExecutePeriodicEffectOnApplication = bExecOnApp;

    // Period / bExecutePeriodicEffectOnApplication are NATIVE UProperties on UGameplayEffect (not BP
    // NewVariables), so values live on the CDO and serialize with the package — mark dirty + save, no
    // recompile (identical rationale to set_gameplay_effect_duration).
    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Blueprint, Edit))
    {
        return SaveFailed;
    }

    const float ReadPeriod = GECDO->Period.GetValue();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("ge_blueprint_path"), BlueprintPath);
    Result->SetNumberField(TEXT("period_seconds"), ReadPeriod);
    Result->SetBoolField(TEXT("period_readback_ok"), FMath::IsNearlyEqual(ReadPeriod, (float)PeriodSeconds, 0.01f));
    Result->SetBoolField(TEXT("execute_on_application"), GECDO->bExecutePeriodicEffectOnApplication);
    Result->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_gameplay_effect_stacking - Set the stacking policy on a GameplayEffect CDO (StackingType + limit + refresh/reset policies). A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param ge_blueprint_path - Full asset path of the GE Blueprint.
//   @param stacking_type - None | AggregateBySource | AggregateByTarget.
//   @param stack_limit - Optional max stack count (int; 0 = no limit).
//   @param duration_refresh_policy - Optional: RefreshOnSuccessfulApplication | NeverRefresh.
//   @param period_reset_policy - Optional: ResetOnSuccessfulApplication | NeverReset.
//   @returns { ge_blueprint_path, stacking_type, stack_limit, stacking_matches, set }
TSharedPtr<FJsonObject> HandleSetGameplayEffectStacking(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ge_blueprint_path"), BlueprintPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString StackingTypeStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("stacking_type"), StackingTypeStr, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    EGameplayEffectStackingType StackType;
    if (StackingTypeStr.Equals(TEXT("None"), ESearchCase::IgnoreCase))                   { StackType = EGameplayEffectStackingType::None; }
    else if (StackingTypeStr.Equals(TEXT("AggregateBySource"), ESearchCase::IgnoreCase)) { StackType = EGameplayEffectStackingType::AggregateBySource; }
    else if (StackingTypeStr.Equals(TEXT("AggregateByTarget"), ESearchCase::IgnoreCase)) { StackType = EGameplayEffectStackingType::AggregateByTarget; }
    else
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid stacking_type '%s' (expected None|AggregateBySource|AggregateByTarget)"), *StackingTypeStr));
    }

    // The optional policies are parsed before anything is written, so a bad value leaves the CDO untouched.
    TOptional<EGameplayEffectStackingDurationPolicy> RefreshPolicy;
    FString RefreshStr;
    if (Params->TryGetStringField(TEXT("duration_refresh_policy"), RefreshStr) && !RefreshStr.IsEmpty())
    {
        if (RefreshStr.Equals(TEXT("RefreshOnSuccessfulApplication"), ESearchCase::IgnoreCase)) { RefreshPolicy = EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication; }
        else if (RefreshStr.Equals(TEXT("NeverRefresh"), ESearchCase::IgnoreCase))              { RefreshPolicy = EGameplayEffectStackingDurationPolicy::NeverRefresh; }
        else { return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid duration_refresh_policy '%s' (expected RefreshOnSuccessfulApplication|NeverRefresh)"), *RefreshStr)); }
    }
    TOptional<EGameplayEffectStackingPeriodPolicy> ResetPolicy;
    FString ResetStr;
    if (Params->TryGetStringField(TEXT("period_reset_policy"), ResetStr) && !ResetStr.IsEmpty())
    {
        if (ResetStr.Equals(TEXT("ResetOnSuccessfulApplication"), ESearchCase::IgnoreCase)) { ResetPolicy = EGameplayEffectStackingPeriodPolicy::ResetOnSuccessfulApplication; }
        else if (ResetStr.Equals(TEXT("NeverReset"), ESearchCase::IgnoreCase))              { ResetPolicy = EGameplayEffectStackingPeriodPolicy::NeverReset; }
        else { return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid period_reset_policy '%s' (expected ResetOnSuccessfulApplication|NeverReset)"), *ResetStr)); }
    }

    UBlueprint* Blueprint = nullptr;
    FString Error;
    UGameplayEffect* GECDO = LoadGameplayEffectCDO(BlueprintPath, Blueprint, Error);
    if (!GECDO)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    if (TSharedPtr<FJsonObject> Refused = GASAuthoring_RefuseUnsavableEdit(Blueprint))
    {
        return Refused;
    }

    FGASAuthoringCDOEdit Edit(GECDO);
    GECDO->Modify();
    // UGameplayEffect::StackingType is UE_DEPRECATED(5.7) ("will be made private; use GetStackingType").
    // The only setter, SetStackingType(), is WITH_EDITOR + non-exported (no GAMEPLAYABILITIES_API) -> not
    // linkable cross-module, so we write the field directly under the deprecation pragma. Read-back below
    // uses the exported GetStackingType().
    PRAGMA_DISABLE_DEPRECATION_WARNINGS
    GECDO->StackingType = StackType;
    PRAGMA_ENABLE_DEPRECATION_WARNINGS

    { double L = 0.0; if (Params->TryGetNumberField(TEXT("stack_limit"), L)) { GECDO->StackLimitCount = (int32)L; } }

    if (RefreshPolicy.IsSet())
    {
        GECDO->StackDurationRefreshPolicy = RefreshPolicy.GetValue();
    }
    if (ResetPolicy.IsSet())
    {
        GECDO->StackPeriodResetPolicy = ResetPolicy.GetValue();
    }

    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Blueprint, Edit))
    {
        return SaveFailed;
    }

    auto StackTypeToStr = [](EGameplayEffectStackingType T) -> FString {
        switch (T)
        {
            case EGameplayEffectStackingType::AggregateBySource: return TEXT("AggregateBySource");
            case EGameplayEffectStackingType::AggregateByTarget: return TEXT("AggregateByTarget");
            default:                                             return TEXT("None");
        } };

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("ge_blueprint_path"), BlueprintPath);
    Result->SetStringField(TEXT("stacking_type"), StackTypeToStr(GECDO->GetStackingType()));
    Result->SetNumberField(TEXT("stack_limit"), GECDO->StackLimitCount);
    Result->SetBoolField(TEXT("stacking_matches"), GECDO->GetStackingType() == StackType);
    Result->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:add_gameplay_effect_modifier - Append one modifier with a constant magnitude to the Modifiers array on the class default object of a GameplayEffect Blueprint, then save it without recompiling and without an undo entry. attribute_name is looked up across every loaded AttributeSet class; an unknown attribute or modifier_op, a running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit. Each call appends a new entry, even an identical one; modifier_index says where it went. Change its magnitude type with set_gameplay_effect_modifier_set_by_caller, set_gameplay_effect_modifier_attribute_based or set_gameplay_effect_modifier_custom; remove_gameplay_effect_modifier drops it; get_gameplay_effect_modifiers lists them.
//   @param ge_blueprint_path - Full asset path of the GE Blueprint
//   @param attribute_name - Attribute to modify, Health or MySet.Health: the bare name or AttributeSetClass.Attribute, case-insensitive, first match among the loaded AttributeSet classes
//   @param modifier_op - Add, Multiply, Divide, or Override (case-insensitive)
//   @param magnitude - Constant magnitude of the modifier, stored as a ScalableFloat without a curve
//   @returns { ge_blueprint_path: string, attribute_name: string, modifier_op: string, magnitude: number, modifier_index: int, modifier_count: int, magnitude_readback_ok: bool, magnitude_readback: number, added: bool }
TSharedPtr<FJsonObject> HandleAddGameplayEffectModifier(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ge_blueprint_path"), BlueprintPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString AttributeName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("attribute_name"), AttributeName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString ModifierOpStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("modifier_op"), ModifierOpStr, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    double Magnitude = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("magnitude"), Magnitude)) { return Missing; }

    // Map modifier op string -> EGameplayModOp values (GameplayEffectTypes.h).
    // Additive=0 (Add), Multiplicitive=1 (Multiply), Division=2 (Divide), Override=3.
    EGameplayModOp::Type ModOp;
    if (ModifierOpStr.Equals(TEXT("Add"), ESearchCase::IgnoreCase))
    {
        ModOp = EGameplayModOp::Additive;
    }
    else if (ModifierOpStr.Equals(TEXT("Multiply"), ESearchCase::IgnoreCase))
    {
        ModOp = EGameplayModOp::Multiplicitive;
    }
    else if (ModifierOpStr.Equals(TEXT("Divide"), ESearchCase::IgnoreCase))
    {
        ModOp = EGameplayModOp::Division;
    }
    else if (ModifierOpStr.Equals(TEXT("Override"), ESearchCase::IgnoreCase))
    {
        ModOp = EGameplayModOp::Override;
    }
    else
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid modifier_op '%s' (expected Add|Multiply|Divide|Override)"), *ModifierOpStr));
    }

    // Resolve the target attribute across all attribute-set classes. This requires at
    // least one UAttributeSet subclass to define the named attribute; engine ships
    // UAbilitySystemTestAttributeSet but a project may not have a usable one — return
    // an honest error if it cannot be resolved.
    FGameplayAttribute Attribute = ResolveAttributeByName(AttributeName);
    if (!Attribute.IsValid())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Attribute not found in any AttributeSet: %s"), *AttributeName));
    }

    UBlueprint* Blueprint = nullptr;
    FString Error;
    UGameplayEffect* GECDO = LoadGameplayEffectCDO(BlueprintPath, Blueprint, Error);
    if (!GECDO)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    if (TSharedPtr<FJsonObject> Refused = GASAuthoring_RefuseUnsavableEdit(Blueprint))
    {
        return Refused;
    }

    FGASAuthoringCDOEdit Edit(GECDO);
    GECDO->Modify();

    // Build and append the modifier (FGameplayModifierInfo, GameplayEffect.h).
    FGameplayModifierInfo ModInfo;
    ModInfo.Attribute = Attribute;
    ModInfo.ModifierOp = ModOp;
    ModInfo.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat((float)Magnitude));

    const int32 IndexAdded = GECDO->Modifiers.Add(ModInfo);

    // Persist the CDO edit without recompiling (Modifiers is a native UProperty on the
    // UGameplayEffect parent; see set_gameplay_effect_duration for rationale).
    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Blueprint, Edit))
    {
        return SaveFailed;
    }

    // Read back the modifier count from the same CDO (no recompile occurred).
    UGameplayEffect* ReadCDO = GECDO;
    const int32 ModifierCount = ReadCDO ? ReadCDO->Modifiers.Num() : (IndexAdded + 1);

    // Confirm the appended modifier's magnitude reads back.
    float ReadMagnitude = 0.f;
    bool bMagReadOk = false;
    if (ReadCDO && ReadCDO->Modifiers.IsValidIndex(IndexAdded))
    {
        bMagReadOk = ReadCDO->Modifiers[IndexAdded].ModifierMagnitude.GetStaticMagnitudeIfPossible(1.0f, ReadMagnitude);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("ge_blueprint_path"), BlueprintPath);
    Result->SetStringField(TEXT("attribute_name"), Attribute.GetName());
    Result->SetStringField(TEXT("modifier_op"), ModifierOpStr);
    Result->SetNumberField(TEXT("magnitude"), Magnitude);
    Result->SetNumberField(TEXT("modifier_index"), IndexAdded);
    Result->SetNumberField(TEXT("modifier_count"), ModifierCount);
    Result->SetBoolField(TEXT("magnitude_readback_ok"), bMagReadOk);
    Result->SetNumberField(TEXT("magnitude_readback"), ReadMagnitude);
    Result->SetBoolField(TEXT("added"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:remove_gameplay_effect_modifier - Remove a modifier from a GameplayEffect CDO's Modifiers array by index (the D in add/set CRUD). A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param ge_blueprint_path - Full asset path of the GE Blueprint
//   @param modifier_index - Index into the GE CDO Modifiers array to remove (0 = first; add_gameplay_effect_modifier appends)
//   @returns { ge_blueprint_path: string, removed_index: int, modifier_count: int, removed: bool }
TSharedPtr<FJsonObject> HandleRemoveGameplayEffectModifier(const TSharedPtr<FJsonObject>& Params)
{
    FGASAuthoringModifier Modifier;
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_ReadModifierTarget(Params, Modifier))
    {
        return Error;
    }

    if (TSharedPtr<FJsonObject> Error = GASAuthoring_LoadGEModifier(Modifier))
    {
        return Error;
    }

    FGASAuthoringCDOEdit Edit(Modifier.GECDO);
    Modifier.GECDO->Modify();

    // Remove the entry. UGameplayEffect::Modifiers is a public native UProperty
    // (TArray<FGameplayModifierInfo>) already mutated directly via .Add() in
    // add_gameplay_effect_modifier — RemoveAt shifts the trailing entries down so the
    // surviving modifiers are re-indexed contiguously (the inverse of the appending add).
    Modifier.GECDO->Modifiers.RemoveAt(Modifier.Index);

    // Persist the CDO edit without recompiling (Modifiers lives on the UGameplayEffect
    // parent CDO and serializes with the package; identical rationale to the add path).
    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Modifier.Blueprint, Edit))
    {
        return SaveFailed;
    }

    // Read back the modifier count from the same CDO (no recompile occurred).
    const int32 ModifierCount = Modifier.GECDO->Modifiers.Num();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("ge_blueprint_path"), Modifier.BlueprintPath);
    Result->SetNumberField(TEXT("removed_index"), Modifier.Index);
    Result->SetNumberField(TEXT("modifier_count"), ModifierCount);
    Result->SetBoolField(TEXT("removed"), true);
    return mcp_common::MakeOkReply(Result);
}

// GAS magnitude types: switch an existing GE modifier's magnitude calculation to
// SetByCaller or AttributeBased (vs the default ScalableFloat from add_gameplay_effect_modifier).
// APIs (GameplayAbilities plugin):
//   FGameplayEffectModifierMagnitude(const FSetByCallerFloat&) / (const FAttributeBasedFloat&)  GameplayEffect.h (public inline ctors)
//   FSetByCallerFloat::{DataTag (FGameplayTag), DataName (FName)}                                 GameplayEffect.h
//   FAttributeBasedFloat::{Coefficient/Pre/PostMultiplyAdditiveValue (FScalableFloat),
//     BackingAttribute (FGameplayEffectAttributeCaptureDefinition), AttributeCalculationType}     GameplayEffect.h
//   FGameplayEffectAttributeCaptureDefinition(FGameplayAttribute, EGameplayEffectAttributeCaptureSource, bSnapshot) (inline)
//   Read-back: GetMagnitudeCalculationType()/GetSetByCallerFloat() (inline), GetAttributeCaptureDefinitions() (UE_API)

// MCP:set_gameplay_effect_modifier_set_by_caller - Switch an existing GE modifier's magnitude to SetByCaller (resolved at runtime by tag/name). A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param ge_blueprint_path - Full asset path of the GE Blueprint
//   @param modifier_index - Index into the GE CDO Modifiers array (add_gameplay_effect_modifier appends; 0 = first)
//   @param data_tag - Optional gameplay tag the caller uses to set the magnitude (modern path; must be a registered tag)
//   @param data_name - Optional legacy FName the caller uses to set the magnitude. At least one of data_tag/data_name is required
//   @returns { ge_blueprint_path: string, modifier_index: int, modifier_count: int, magnitude_type: string, data_tag: string, data_tag_valid: bool, data_name: string, set: bool }
TSharedPtr<FJsonObject> HandleSetGameplayEffectModifierSetByCaller(const TSharedPtr<FJsonObject>& Params)
{
    FGASAuthoringModifier Modifier;
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_ReadModifierTarget(Params, Modifier))
    {
        return Error;
    }

    FString DataTagStr;
    Params->TryGetStringField(TEXT("data_tag"), DataTagStr);
    DataTagStr.TrimStartAndEndInline();
    FString DataNameStr;
    Params->TryGetStringField(TEXT("data_name"), DataNameStr);
    DataNameStr.TrimStartAndEndInline();
    if (DataTagStr.IsEmpty() && DataNameStr.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Provide at least one of data_tag or data_name"));
    }

    if (TSharedPtr<FJsonObject> Error = GASAuthoring_LoadGEModifier(Modifier))
    {
        return Error;
    }

    // Build a SetByCaller magnitude. DataTag is the modern path (must be a registered
    // gameplay tag); DataName is the legacy free-form FName path (no registration). At
    // least one must end up set or the magnitude is unresolvable at runtime.
    FSetByCallerFloat SetByCaller;
    bool bTagValid = false;
    if (!DataTagStr.IsEmpty())
    {
        const FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(FName(*DataTagStr), /*ErrorIfNotFound=*/false);
        if (Tag.IsValid())
        {
            SetByCaller.DataTag = Tag;
            bTagValid = true;
        }
    }
    if (!DataNameStr.IsEmpty())
    {
        SetByCaller.DataName = FName(*DataNameStr);
    }
    if (!bTagValid && DataNameStr.IsEmpty())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("data_tag '%s' is not a registered gameplay tag and no data_name was given"), *DataTagStr));
    }

    FGASAuthoringCDOEdit Edit(Modifier.GECDO);
    Modifier.GECDO->Modify();
    // The public inline ctor FGameplayEffectModifierMagnitude(const FSetByCallerFloat&) sets
    // MagnitudeCalculationType = SetByCaller (GameplayEffect.h). The struct's members
    // are protected (friend UGameplayEffect) but assigning a freshly-built magnitude needs no
    // friend access. ModifierMagnitude is a native UProperty on the parent -> mark dirty + save,
    // no recompile (identical rationale to set_gameplay_effect_duration).
    Modifier.Magnitude() = FGameplayEffectModifierMagnitude(SetByCaller);

    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Modifier.Blueprint, Edit))
    {
        return SaveFailed;
    }

    // Read back from the CDO (no recompile). GetMagnitudeCalculationType()/GetSetByCallerFloat() are inline.
    const FGameplayEffectModifierMagnitude& Mag = Modifier.Magnitude();
    const FSetByCallerFloat& OutSBC = Mag.GetSetByCallerFloat();
    const bool bIsSetByCaller = Mag.GetMagnitudeCalculationType() == EGameplayEffectMagnitudeCalculation::SetByCaller;

    TSharedPtr<FJsonObject> Result = GASAuthoring_ModifierResult(Modifier, bIsSetByCaller ? TEXT("SetByCaller") : TEXT("Unknown"));
    Result->SetStringField(TEXT("data_tag"), OutSBC.DataTag.ToString());
    Result->SetBoolField(TEXT("data_tag_valid"), bTagValid);
    Result->SetStringField(TEXT("data_name"), OutSBC.DataName.ToString());
    Result->SetBoolField(TEXT("set"), bIsSetByCaller);
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_gameplay_effect_modifier_attribute_based - Switch an existing GE modifier's magnitude to AttributeBased (scales off a captured attribute). A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param ge_blueprint_path - Full asset path of the GE Blueprint
//   @param modifier_index - Index into the GE CDO Modifiers array (0 = first)
//   @param backing_attribute - Attribute to capture, e.g. Health or MySet.Health (must exist on some AttributeSet)
//   @param attribute_source - Capture source: Source (caster) or Target (recipient). Default Source
//   @param coefficient - Multiplier applied to the captured value. Default 1.0
//   @param pre_multiply_add - Additive value applied before the coefficient. Default 0.0
//   @param post_multiply_add - Additive value applied after the coefficient. Default 0.0
//   @param calculation_type - AttributeMagnitude | AttributeBaseValue | AttributeBonusMagnitude. Default AttributeMagnitude
//   @param snapshot - If true capture the attribute at spec creation, else track changes. Default true
//   @returns { ge_blueprint_path: string, modifier_index: int, modifier_count: int, magnitude_type: string, backing_attribute: string, capture_attribute_readback: string, backing_attribute_matches: bool, attribute_source: string, coefficient: number, pre_multiply_add: number, post_multiply_add: number, calculation_type: string, snapshot: bool, set: bool }
TSharedPtr<FJsonObject> HandleSetGameplayEffectModifierAttributeBased(const TSharedPtr<FJsonObject>& Params)
{
    FGASAuthoringModifier Modifier;
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_ReadModifierTarget(Params, Modifier))
    {
        return Error;
    }

    FString BackingAttrName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("backing_attribute"), BackingAttrName)) { return Missing; }

    // Capture source (default Source = caster).
    EGameplayEffectAttributeCaptureSource CaptureSource = EGameplayEffectAttributeCaptureSource::Source;
    FString SourceStr;
    if (Params->TryGetStringField(TEXT("attribute_source"), SourceStr) && !SourceStr.IsEmpty())
    {
        if (SourceStr.Equals(TEXT("Source"), ESearchCase::IgnoreCase))      { CaptureSource = EGameplayEffectAttributeCaptureSource::Source; }
        else if (SourceStr.Equals(TEXT("Target"), ESearchCase::IgnoreCase)) { CaptureSource = EGameplayEffectAttributeCaptureSource::Target; }
        else { return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid attribute_source '%s' (expected Source|Target)"), *SourceStr)); }
    }

    // Calculation policy (default AttributeMagnitude). We expose the three context-free
    // policies; AttributeMagnitudeEvaluatedUpToChannel needs a FinalChannel and is omitted.
    EAttributeBasedFloatCalculationType CalcType = EAttributeBasedFloatCalculationType::AttributeMagnitude;
    FString CalcStr;
    if (Params->TryGetStringField(TEXT("calculation_type"), CalcStr) && !CalcStr.IsEmpty())
    {
        if (CalcStr.Equals(TEXT("AttributeMagnitude"), ESearchCase::IgnoreCase))           { CalcType = EAttributeBasedFloatCalculationType::AttributeMagnitude; }
        else if (CalcStr.Equals(TEXT("AttributeBaseValue"), ESearchCase::IgnoreCase))      { CalcType = EAttributeBasedFloatCalculationType::AttributeBaseValue; }
        else if (CalcStr.Equals(TEXT("AttributeBonusMagnitude"), ESearchCase::IgnoreCase)) { CalcType = EAttributeBasedFloatCalculationType::AttributeBonusMagnitude; }
        else { return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid calculation_type '%s' (expected AttributeMagnitude|AttributeBaseValue|AttributeBonusMagnitude)"), *CalcStr)); }
    }

    double Coefficient = 1.0; Params->TryGetNumberField(TEXT("coefficient"), Coefficient);
    double PreAdd = 0.0;      Params->TryGetNumberField(TEXT("pre_multiply_add"), PreAdd);
    double PostAdd = 0.0;     Params->TryGetNumberField(TEXT("post_multiply_add"), PostAdd);
    bool bSnapshot = true;    Params->TryGetBoolField(TEXT("snapshot"), bSnapshot);

    // Resolve the backing attribute across all attribute-set classes (same helper as add_gameplay_effect_modifier).
    FGameplayAttribute BackingAttr = ResolveAttributeByName(BackingAttrName);
    if (!BackingAttr.IsValid())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Attribute not found in any AttributeSet: %s"), *BackingAttrName));
    }

    if (TSharedPtr<FJsonObject> Error = GASAuthoring_LoadGEModifier(Modifier))
    {
        return Error;
    }

    // Build the attribute-based float. All members are public; the capture-definition ctor
    // (FGameplayAttribute, EGameplayEffectAttributeCaptureSource, bSnapshot) is inline
    // (GameplayEffectAttributeCaptureDefinition.h). BackingAttr is passed by value
    // (copied), so it stays valid for the read-back below.
    FAttributeBasedFloat AttrFloat;
    AttrFloat.Coefficient = FScalableFloat((float)Coefficient);
    AttrFloat.PreMultiplyAdditiveValue = FScalableFloat((float)PreAdd);
    AttrFloat.PostMultiplyAdditiveValue = FScalableFloat((float)PostAdd);
    AttrFloat.BackingAttribute = FGameplayEffectAttributeCaptureDefinition(BackingAttr, CaptureSource, bSnapshot);
    AttrFloat.AttributeCalculationType = CalcType;

    FGASAuthoringCDOEdit Edit(Modifier.GECDO);
    Modifier.GECDO->Modify();
    Modifier.Magnitude() = FGameplayEffectModifierMagnitude(AttrFloat);

    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Modifier.Blueprint, Edit))
    {
        return SaveFailed;
    }

    // Read back: confirm type + the captured backing attribute via the exported
    // GetAttributeCaptureDefinitions (UE_API).
    const FGameplayEffectModifierMagnitude& Mag = Modifier.Magnitude();
    const bool bIsAttrBased = Mag.GetMagnitudeCalculationType() == EGameplayEffectMagnitudeCalculation::AttributeBased;
    const FString CaptureReadback = GASAuthoring_CapturedAttributeName(Mag);

    auto SourceToStr = [](EGameplayEffectAttributeCaptureSource S) -> FString {
        return S == EGameplayEffectAttributeCaptureSource::Target ? TEXT("Target") : TEXT("Source"); };
    auto CalcToStr = [](EAttributeBasedFloatCalculationType C) -> FString {
        switch (C)
        {
            case EAttributeBasedFloatCalculationType::AttributeBaseValue:      return TEXT("AttributeBaseValue");
            case EAttributeBasedFloatCalculationType::AttributeBonusMagnitude: return TEXT("AttributeBonusMagnitude");
            case EAttributeBasedFloatCalculationType::AttributeMagnitudeEvaluatedUpToChannel: return TEXT("AttributeMagnitudeEvaluatedUpToChannel");
            default:                                                           return TEXT("AttributeMagnitude");
        } };

    TSharedPtr<FJsonObject> Result = GASAuthoring_ModifierResult(Modifier, bIsAttrBased ? TEXT("AttributeBased") : TEXT("Unknown"));
    Result->SetStringField(TEXT("backing_attribute"), BackingAttr.GetName());
    Result->SetStringField(TEXT("capture_attribute_readback"), CaptureReadback);
    Result->SetBoolField(TEXT("backing_attribute_matches"), !CaptureReadback.IsEmpty() && CaptureReadback == BackingAttr.GetName());
    Result->SetStringField(TEXT("attribute_source"), SourceToStr(CaptureSource));
    Result->SetNumberField(TEXT("coefficient"), Coefficient);
    Result->SetNumberField(TEXT("pre_multiply_add"), PreAdd);
    Result->SetNumberField(TEXT("post_multiply_add"), PostAdd);
    Result->SetStringField(TEXT("calculation_type"), CalcToStr(CalcType));
    Result->SetBoolField(TEXT("snapshot"), bSnapshot);
    Result->SetBoolField(TEXT("set"), bIsAttrBased);
    return mcp_common::MakeOkReply(Result);
}

// MCP:create_gameplay_mod_magnitude_calculation_blueprint - Create a Blueprint parented to UGameplayModMagnitudeCalculation (a custom magnitude calc class for GE modifiers), compile and save it. A Blueprint of that class already at the path is returned untouched (created false, already_existed true); one of another class there is an error.
//   @param name - Asset name, e.g. MMC_Damage
//   @param package_path - Target content path, defaults to /Game/Blueprints
//   @returns { name: string, asset_path: string, parent_class: string, generated_class: string, is_mod_magnitude_calculation: bool, already_existed: bool, created: bool }
TSharedPtr<FJsonObject> HandleCreateGameplayModMagnitudeCalculationBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    // UGameplayModMagnitudeCalculation is Abstract + Blueprintable (a UObject, not an Actor). CreateBlueprint
    // makes a CONCRETE BP subclass of it — the same UObject-parent path create_attribute_set_blueprint uses,
    // so it sidesteps the Actor-only limitation of the generic Blueprint-domain create_blueprint command.
    FGASAuthoringCreatedBlueprint Created;
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_CreateBlueprint(Params, UGameplayModMagnitudeCalculation::StaticClass(), Created))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Result = GASAuthoring_CreatedBlueprintFields(Created, TEXT("GameplayModMagnitudeCalculation"), TEXT("is_mod_magnitude_calculation"));
    Result->SetBoolField(TEXT("already_existed"), Created.bAlreadyExisted);
    Result->SetBoolField(TEXT("created"), !Created.bAlreadyExisted);
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_gameplay_effect_modifier_custom - Switch an existing GE modifier's magnitude to CustomCalculationClass (a UGameplayModMagnitudeCalculation subclass computes it). A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param ge_blueprint_path - Full asset path of the GE Blueprint
//   @param modifier_index - Index into the GE CDO Modifiers array (0 = first)
//   @param calculation_class - BP asset path (/Game/...) or class name of a CONCRETE UGameplayModMagnitudeCalculation subclass
//   @param coefficient - Multiplier applied to the calc output. Default 1.0
//   @param pre_multiply_add - Additive value applied before the coefficient. Default 0.0
//   @param post_multiply_add - Additive value applied after the coefficient. Default 0.0
//   @returns { ge_blueprint_path: string, modifier_index: int, modifier_count: int, magnitude_type: string, calculation_class: string, calculation_class_matches: bool, coefficient: number, pre_multiply_add: number, post_multiply_add: number, set: bool }
TSharedPtr<FJsonObject> HandleSetGameplayEffectModifierCustom(const TSharedPtr<FJsonObject>& Params)
{
    FGASAuthoringModifier Modifier;
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_ReadModifierTarget(Params, Modifier))
    {
        return Error;
    }
    FString CalcClassStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("calculation_class"), CalcClassStr)) { return Missing; }

    double Coefficient = 1.0; Params->TryGetNumberField(TEXT("coefficient"), Coefficient);
    double PreAdd = 0.0;      Params->TryGetNumberField(TEXT("pre_multiply_add"), PreAdd);
    double PostAdd = 0.0;     Params->TryGetNumberField(TEXT("post_multiply_add"), PostAdd);

    UClass* CalcClass = ResolveModMagnitudeCalcClass(CalcClassStr);
    if (!CalcClass)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("calculation_class not found: %s"), *CalcClassStr));
    }
    if (!CalcClass->IsChildOf(UGameplayModMagnitudeCalculation::StaticClass()))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("calculation_class is not a UGameplayModMagnitudeCalculation subclass: %s"), *CalcClassStr));
    }
    if (CalcClass->HasAnyClassFlags(CLASS_Abstract))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("calculation_class is Abstract (cannot be instantiated at runtime): %s"), *CalcClassStr));
    }

    if (TSharedPtr<FJsonObject> Error = GASAuthoring_LoadGEModifier(Modifier))
    {
        return Error;
    }

    // Build the custom-calculation float (public members; FGameplayEffect.h). The public inline ctor
    // FGameplayEffectModifierMagnitude(const FCustomCalculationBasedFloat&) sets MagnitudeCalculationType =
    // CustomCalculationClass (GameplayEffect.h). CalculationClassMagnitude is TSubclassOf<...> (verified above).
    FCustomCalculationBasedFloat CustomFloat;
    CustomFloat.CalculationClassMagnitude = CalcClass;
    CustomFloat.Coefficient = FScalableFloat((float)Coefficient);
    CustomFloat.PreMultiplyAdditiveValue = FScalableFloat((float)PreAdd);
    CustomFloat.PostMultiplyAdditiveValue = FScalableFloat((float)PostAdd);

    FGASAuthoringCDOEdit Edit(Modifier.GECDO);
    Modifier.GECDO->Modify();
    Modifier.Magnitude() = FGameplayEffectModifierMagnitude(CustomFloat);

    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Modifier.Blueprint, Edit))
    {
        return SaveFailed;
    }

    // Read back: type + the calc class via the exported GetCustomMagnitudeCalculationClass (UE_API).
    const FGameplayEffectModifierMagnitude& Mag = Modifier.Magnitude();
    const bool bIsCustom = Mag.GetMagnitudeCalculationType() == EGameplayEffectMagnitudeCalculation::CustomCalculationClass;
    UClass* RBClass = Mag.GetCustomMagnitudeCalculationClass().Get();
    const FString RBName = RBClass ? RBClass->GetName() : FString();

    TSharedPtr<FJsonObject> Result = GASAuthoring_ModifierResult(Modifier, bIsCustom ? TEXT("CustomCalculationClass") : TEXT("Unknown"));
    Result->SetStringField(TEXT("calculation_class"), CalcClass->GetName());
    Result->SetBoolField(TEXT("calculation_class_matches"), RBClass == CalcClass);
    Result->SetNumberField(TEXT("coefficient"), Coefficient);
    Result->SetNumberField(TEXT("pre_multiply_add"), PreAdd);
    Result->SetNumberField(TEXT("post_multiply_add"), PostAdd);
    Result->SetBoolField(TEXT("set"), bIsCustom);
    return mcp_common::MakeOkReply(Result);
}

// GAS magnitude extras (refinements of the AttributeBased / Custom magnitudes above).
// APIs (GameplayAbilities plugin):
//   EAttributeBasedFloatCalculationType::AttributeMagnitudeEvaluatedUpToChannel + FAttributeBasedFloat::FinalChannel
//     (EGameplayModEvaluationChannel Channel0..Channel9)                                   GameplayEffect.h / GameplayEffectTypes.h
//   FCustomCalculationBasedFloat::FinalLookupCurve (FCurveTableRowHandle)                   GameplayEffect.h
//   The protected inner sub-structs AttributeBasedMagnitude/CustomMagnitude are reached by
//   reflection (GetMagnitudeSubStructPtr, by-name UScriptStruct lookup) — read-back of channel/calc-type
//   and the FinalLookupCurve write/read (FinalLookupCurve is a public member once the struct is reached).

// MCP:set_gameplay_effect_modifier_attribute_based_up_to_channel - Switch a GE modifier's magnitude to AttributeBased with the channel-terminated calculation policy (AttributeMagnitudeEvaluatedUpToChannel + FinalChannel). A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param ge_blueprint_path - Full asset path of the GE Blueprint
//   @param modifier_index - Index into the GE CDO Modifiers array (0 = first)
//   @param backing_attribute - Attribute to capture, e.g. Health or MySet.Health (must exist on some AttributeSet)
//   @param final_channel - Mod evaluation channel (0-9) to terminate the partial attribute evaluation at. Default 0
//   @param attribute_source - Capture source: Source (caster) or Target (recipient). Default Source
//   @param coefficient - Multiplier applied to the captured value. Default 1.0
//   @param pre_multiply_add - Additive value applied before the coefficient. Default 0.0
//   @param post_multiply_add - Additive value applied after the coefficient. Default 0.0
//   @param snapshot - If true capture the attribute at spec creation, else track changes. Default true
//   @returns { ge_blueprint_path: string, modifier_index: int, modifier_count: int, magnitude_type: string, backing_attribute: string, capture_attribute_readback: string, backing_attribute_matches: bool, attribute_source: string, coefficient: number, pre_multiply_add: number, post_multiply_add: number, calculation_type: string, final_channel: int, final_channel_readback: int, channel_matches: bool, snapshot: bool, set: bool }
TSharedPtr<FJsonObject> HandleSetGameplayEffectModifierAttributeBasedUpToChannel(const TSharedPtr<FJsonObject>& Params)
{
    FGASAuthoringModifier Modifier;
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_ReadModifierTarget(Params, Modifier))
    {
        return Error;
    }

    FString BackingAttrName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("backing_attribute"), BackingAttrName)) { return Missing; }

    // Final channel (0-9). EGameplayModEvaluationChannel is a contiguous uint8 enum
    // Channel0..Channel9 (GameplayEffectTypes.h); Channel_MAX is the count. Default Channel0.
    double FinalChannelD = 0.0; Params->TryGetNumberField(TEXT("final_channel"), FinalChannelD);
    const int32 FinalChannelInt = (int32)FinalChannelD;
    if (FinalChannelInt < 0 || FinalChannelInt >= (int32)EGameplayModEvaluationChannel::Channel_MAX)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid final_channel %d (expected 0..%d)"),
                FinalChannelInt, (int32)EGameplayModEvaluationChannel::Channel_MAX - 1));
    }
    const EGameplayModEvaluationChannel FinalChannel = (EGameplayModEvaluationChannel)FinalChannelInt;

    // Capture source (default Source = caster).
    EGameplayEffectAttributeCaptureSource CaptureSource = EGameplayEffectAttributeCaptureSource::Source;
    FString SourceStr;
    if (Params->TryGetStringField(TEXT("attribute_source"), SourceStr) && !SourceStr.IsEmpty())
    {
        if (SourceStr.Equals(TEXT("Source"), ESearchCase::IgnoreCase))      { CaptureSource = EGameplayEffectAttributeCaptureSource::Source; }
        else if (SourceStr.Equals(TEXT("Target"), ESearchCase::IgnoreCase)) { CaptureSource = EGameplayEffectAttributeCaptureSource::Target; }
        else { return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid attribute_source '%s' (expected Source|Target)"), *SourceStr)); }
    }

    double Coefficient = 1.0; Params->TryGetNumberField(TEXT("coefficient"), Coefficient);
    double PreAdd = 0.0;      Params->TryGetNumberField(TEXT("pre_multiply_add"), PreAdd);
    double PostAdd = 0.0;     Params->TryGetNumberField(TEXT("post_multiply_add"), PostAdd);
    bool bSnapshot = true;    Params->TryGetBoolField(TEXT("snapshot"), bSnapshot);

    FGameplayAttribute BackingAttr = ResolveAttributeByName(BackingAttrName);
    if (!BackingAttr.IsValid())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Attribute not found in any AttributeSet: %s"), *BackingAttrName));
    }

    if (TSharedPtr<FJsonObject> Error = GASAuthoring_LoadGEModifier(Modifier))
    {
        return Error;
    }

    // Build the attribute-based float with the channel-terminated policy. All members are public;
    // FinalChannel is only meaningful for AttributeMagnitudeEvaluatedUpToChannel (mirrors the
    // set_gameplay_effect_modifier_attribute_based command, which intentionally omits this policy).
    FAttributeBasedFloat AttrFloat;
    AttrFloat.Coefficient = FScalableFloat((float)Coefficient);
    AttrFloat.PreMultiplyAdditiveValue = FScalableFloat((float)PreAdd);
    AttrFloat.PostMultiplyAdditiveValue = FScalableFloat((float)PostAdd);
    AttrFloat.BackingAttribute = FGameplayEffectAttributeCaptureDefinition(BackingAttr, CaptureSource, bSnapshot);
    AttrFloat.AttributeCalculationType = EAttributeBasedFloatCalculationType::AttributeMagnitudeEvaluatedUpToChannel;
    AttrFloat.FinalChannel = FinalChannel;

    FGASAuthoringCDOEdit Edit(Modifier.GECDO);
    Modifier.GECDO->Modify();
    Modifier.Magnitude() = FGameplayEffectModifierMagnitude(AttrFloat);

    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Modifier.Blueprint, Edit))
    {
        return SaveFailed;
    }

    // Read back: type via the exported getter, captured attribute via GetAttributeCaptureDefinitions,
    // and the calc-type + channel via reflection into the protected AttributeBasedMagnitude sub-struct.
    FGameplayEffectModifierMagnitude& Mag = Modifier.Magnitude();
    const bool bIsAttrBased = Mag.GetMagnitudeCalculationType() == EGameplayEffectMagnitudeCalculation::AttributeBased;
    const FString CaptureReadback = GASAuthoring_CapturedAttributeName(Mag);

    EAttributeBasedFloatCalculationType RBCalcType = EAttributeBasedFloatCalculationType::AttributeMagnitude;
    int32 RBChannel = -1;
    if (const FAttributeBasedFloat* RBAttr = GetMagnitudeSubStructPtr<FAttributeBasedFloat>(Mag, TEXT("AttributeBasedMagnitude")))
    {
        RBCalcType = RBAttr->AttributeCalculationType;
        RBChannel = (int32)RBAttr->FinalChannel;
    }
    const bool bCalcTypeOk = RBCalcType == EAttributeBasedFloatCalculationType::AttributeMagnitudeEvaluatedUpToChannel;
    const bool bChannelMatches = RBChannel == FinalChannelInt;

    auto SourceToStr = [](EGameplayEffectAttributeCaptureSource S) -> FString {
        return S == EGameplayEffectAttributeCaptureSource::Target ? TEXT("Target") : TEXT("Source"); };

    TSharedPtr<FJsonObject> Result = GASAuthoring_ModifierResult(Modifier, bIsAttrBased ? TEXT("AttributeBased") : TEXT("Unknown"));
    Result->SetStringField(TEXT("backing_attribute"), BackingAttr.GetName());
    Result->SetStringField(TEXT("capture_attribute_readback"), CaptureReadback);
    Result->SetBoolField(TEXT("backing_attribute_matches"), !CaptureReadback.IsEmpty() && CaptureReadback == BackingAttr.GetName());
    Result->SetStringField(TEXT("attribute_source"), SourceToStr(CaptureSource));
    Result->SetNumberField(TEXT("coefficient"), Coefficient);
    Result->SetNumberField(TEXT("pre_multiply_add"), PreAdd);
    Result->SetNumberField(TEXT("post_multiply_add"), PostAdd);
    Result->SetStringField(TEXT("calculation_type"), bCalcTypeOk ? TEXT("AttributeMagnitudeEvaluatedUpToChannel") : TEXT("Unknown"));
    Result->SetNumberField(TEXT("final_channel"), FinalChannelInt);
    Result->SetNumberField(TEXT("final_channel_readback"), RBChannel);
    Result->SetBoolField(TEXT("channel_matches"), bChannelMatches);
    Result->SetBoolField(TEXT("snapshot"), bSnapshot);
    Result->SetBoolField(TEXT("set"), bIsAttrBased && bCalcTypeOk && bChannelMatches);
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_gameplay_effect_modifier_custom_curve - Set the FinalLookupCurve (a CurveTable row) on a GE modifier whose magnitude is already CustomCalculationClass; the custom calc output is remapped through the curve. A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param ge_blueprint_path - Full asset path of the GE Blueprint
//   @param modifier_index - Index into the GE CDO Modifiers array; the modifier's magnitude must already be CustomCalculationClass (call set_gameplay_effect_modifier_custom first)
//   @param curve_table_path - Full asset path of the UCurveTable holding the lookup row
//   @param row_name - Name of the row within the CurveTable to use as the final lookup curve
//   @returns { ge_blueprint_path: string, modifier_index: int, modifier_count: int, magnitude_type: string, calculation_class: string, curve_table_path: string, curve_table_readback: string, curve_table_matches: bool, row_name: string, row_name_readback: string, row_name_matches: bool, row_exists: bool, set: bool }
TSharedPtr<FJsonObject> HandleSetGameplayEffectModifierCustomCurve(const TSharedPtr<FJsonObject>& Params)
{
    FGASAuthoringModifier Modifier;
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_ReadModifierTarget(Params, Modifier))
    {
        return Error;
    }
    UCurveTable* CurveTable = nullptr;
    FString RowNameStr;
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_ReadCurveTableRow(Params, CurveTable, RowNameStr))
    {
        return Error;
    }
    const FName RowFName(*RowNameStr);

    if (TSharedPtr<FJsonObject> Error = GASAuthoring_LoadGEModifier(Modifier))
    {
        return Error;
    }

    FGameplayEffectModifierMagnitude& Mag = Modifier.Magnitude();
    if (Mag.GetMagnitudeCalculationType() != EGameplayEffectMagnitudeCalculation::CustomCalculationClass)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("modifier_index %d is not a CustomCalculationClass magnitude; call set_gameplay_effect_modifier_custom first"),
                Modifier.Index));
    }

    // The custom sub-struct (FCustomCalculationBasedFloat CustomMagnitude) is a protected UPROPERTY;
    // reach it by reflection and set only its public FinalLookupCurve handle, preserving the calc
    // class + coefficients already configured (composable with set_gameplay_effect_modifier_custom).
    FCustomCalculationBasedFloat* CustomPtr = GetMagnitudeSubStructPtr<FCustomCalculationBasedFloat>(Mag, TEXT("CustomMagnitude"));
    if (!CustomPtr)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to access CustomMagnitude sub-struct via reflection"));
    }

    const bool bRowExists = CurveTable->FindCurveUnchecked(RowFName) != nullptr;

    FGASAuthoringCDOEdit Edit(Modifier.GECDO);
    Modifier.GECDO->Modify();
    CustomPtr->FinalLookupCurve.CurveTable = CurveTable;
    CustomPtr->FinalLookupCurve.RowName = RowFName;

    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Modifier.Blueprint, Edit))
    {
        return SaveFailed;
    }

    // Read back via the same reflected sub-struct: curve table + row name round-trip; the calc
    // class is read via the exported getter to confirm it survived the curve write.
    const FCustomCalculationBasedFloat* RB = GetMagnitudeSubStructPtr<FCustomCalculationBasedFloat>(Mag, TEXT("CustomMagnitude"));
    UClass* CalcClass = Mag.GetCustomMagnitudeCalculationClass().Get();

    TSharedPtr<FJsonObject> Result = GASAuthoring_ModifierResult(Modifier, TEXT("CustomCalculationClass"));
    Result->SetStringField(TEXT("calculation_class"), CalcClass ? CalcClass->GetName() : FString());
    GASAuthoring_AddCurveRowFields(Result, CurveTable, RowNameStr, RB ? &RB->FinalLookupCurve : nullptr, bRowExists);
    return mcp_common::MakeOkReply(Result);
}

// FAttributeBasedFloat authoring (refinements of the AttributeBased magnitude above):
//   FAttributeBasedFloat::AttributeCurve (FCurveTableRowHandle; remaps the captured attribute through a
//     curve instead of using it directly)                                                   GameplayEffect.h
//   FAttributeBasedFloat::Source/TargetTagFilter (FGameplayTagContainer)                     GameplayEffect.h
//   Both reach the protected AttributeBasedMagnitude sub-struct by reflection (GetMagnitudeSubStructPtr)
//   and write its public members.

// MCP:set_gameplay_effect_modifier_attribute_curve - Set the AttributeCurve (a CurveTable row) on a GE modifier whose magnitude is already AttributeBased; the captured attribute is remapped through the curve instead of used directly. A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param ge_blueprint_path - Full asset path of the GE Blueprint
//   @param modifier_index - Index into the GE CDO Modifiers array; the modifier's magnitude must already be AttributeBased (call set_gameplay_effect_modifier_attribute_based first)
//   @param curve_table_path - Full asset path of the UCurveTable holding the lookup row
//   @param row_name - Name of the row within the CurveTable to use as the attribute lookup curve
//   @returns { ge_blueprint_path: string, modifier_index: int, modifier_count: int, magnitude_type: string, backing_attribute: string, curve_table_path: string, curve_table_readback: string, curve_table_matches: bool, row_name: string, row_name_readback: string, row_name_matches: bool, row_exists: bool, set: bool }
TSharedPtr<FJsonObject> HandleSetGameplayEffectModifierAttributeCurve(const TSharedPtr<FJsonObject>& Params)
{
    FGASAuthoringModifier Modifier;
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_ReadModifierTarget(Params, Modifier))
    {
        return Error;
    }
    UCurveTable* CurveTable = nullptr;
    FString RowNameStr;
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_ReadCurveTableRow(Params, CurveTable, RowNameStr))
    {
        return Error;
    }
    const FName RowFName(*RowNameStr);

    if (TSharedPtr<FJsonObject> Error = GASAuthoring_LoadGEModifier(Modifier))
    {
        return Error;
    }

    FGameplayEffectModifierMagnitude& Mag = Modifier.Magnitude();
    if (Mag.GetMagnitudeCalculationType() != EGameplayEffectMagnitudeCalculation::AttributeBased)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("modifier_index %d is not an AttributeBased magnitude; call set_gameplay_effect_modifier_attribute_based first"),
                Modifier.Index));
    }

    // The attribute-based sub-struct (FAttributeBasedFloat AttributeBasedMagnitude) is a protected
    // UPROPERTY; reach it by reflection and set only its public AttributeCurve handle, preserving the
    // backing attribute + coefficients already configured (composable with
    // set_gameplay_effect_modifier_attribute_based). Mirror of set_gameplay_effect_modifier_custom_curve.
    FAttributeBasedFloat* AttrPtr = GetMagnitudeSubStructPtr<FAttributeBasedFloat>(Mag, TEXT("AttributeBasedMagnitude"));
    if (!AttrPtr)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to access AttributeBasedMagnitude sub-struct via reflection"));
    }

    const bool bRowExists = CurveTable->FindCurveUnchecked(RowFName) != nullptr;

    FGASAuthoringCDOEdit Edit(Modifier.GECDO);
    Modifier.GECDO->Modify();
    AttrPtr->AttributeCurve.CurveTable = CurveTable;
    AttrPtr->AttributeCurve.RowName = RowFName;

    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Modifier.Blueprint, Edit))
    {
        return SaveFailed;
    }

    // Read back via the same reflected sub-struct: curve table + row name round-trip; the backing
    // attribute is read to confirm the AttributeBased config survived the curve write.
    const FAttributeBasedFloat* RB = GetMagnitudeSubStructPtr<FAttributeBasedFloat>(Mag, TEXT("AttributeBasedMagnitude"));

    TSharedPtr<FJsonObject> Result = GASAuthoring_ModifierResult(Modifier, TEXT("AttributeBased"));
    Result->SetStringField(TEXT("backing_attribute"), RB ? RB->BackingAttribute.AttributeToCapture.GetName() : FString());
    GASAuthoring_AddCurveRowFields(Result, CurveTable, RowNameStr, RB ? &RB->AttributeCurve : nullptr, bRowExists);
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_gameplay_effect_modifier_attribute_tag_filters - Set the Source/Target tag filters on a GE modifier whose magnitude is already AttributeBased; only modifiers carrying all of a filter's tags factor into the captured attribute's evaluation. A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param ge_blueprint_path - Full asset path of the GE Blueprint
//   @param modifier_index - Index into the GE CDO Modifiers array; the modifier's magnitude must already be AttributeBased (call set_gameplay_effect_modifier_attribute_based first)
//   @param source_tags - Optional list of gameplay tag strings; replaces SourceTagFilter. Omit to leave the source filter unchanged
//   @param target_tags - Optional list of gameplay tag strings; replaces TargetTagFilter. Omit to leave the target filter unchanged
//   @returns { ge_blueprint_path: string, modifier_index: int, modifier_count: int, magnitude_type: string, source_tags_set: bool, target_tags_set: bool, source_resolved_tags: [string], source_unknown_tags: [string], target_resolved_tags: [string], target_unknown_tags: [string], source_tag_filter: [string], target_tag_filter: [string], set: bool }
TSharedPtr<FJsonObject> HandleSetGameplayEffectModifierAttributeTagFilters(const TSharedPtr<FJsonObject>& Params)
{
    FGASAuthoringModifier Modifier;
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_ReadModifierTarget(Params, Modifier))
    {
        return Error;
    }

    const TArray<TSharedPtr<FJsonValue>>* SourceTagsArray = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* TargetTagsArray = nullptr;
    const bool bHasSource = Params->TryGetArrayField(TEXT("source_tags"), SourceTagsArray) && SourceTagsArray;
    const bool bHasTarget = Params->TryGetArrayField(TEXT("target_tags"), TargetTagsArray) && TargetTagsArray;
    if (!bHasSource && !bHasTarget)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Provide at least one of source_tags / target_tags (arrays of gameplay tag strings)"));
    }

    if (TSharedPtr<FJsonObject> Error = GASAuthoring_LoadGEModifier(Modifier))
    {
        return Error;
    }

    FGameplayEffectModifierMagnitude& Mag = Modifier.Magnitude();
    if (Mag.GetMagnitudeCalculationType() != EGameplayEffectMagnitudeCalculation::AttributeBased)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("modifier_index %d is not an AttributeBased magnitude; call set_gameplay_effect_modifier_attribute_based first"),
                Modifier.Index));
    }

    FAttributeBasedFloat* AttrPtr = GetMagnitudeSubStructPtr<FAttributeBasedFloat>(Mag, TEXT("AttributeBasedMagnitude"));
    if (!AttrPtr)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to access AttributeBasedMagnitude sub-struct via reflection"));
    }

    // Each provided list fully REPLACES its filter (set-to-fixed-value, idempotent); an omitted list
    // leaves that filter untouched (composable refinement).
    FGameplayTagContainer SourceContainer;
    FGameplayTagContainer TargetContainer;
    TArray<TSharedPtr<FJsonValue>> SourceResolved, SourceUnknown, TargetResolved, TargetUnknown;
    FGASAuthoringCDOEdit Edit(Modifier.GECDO);
    Modifier.GECDO->Modify();
    if (bHasSource)
    {
        GASAuthoring_ParseTags(*SourceTagsArray, SourceContainer, SourceResolved, SourceUnknown);
        AttrPtr->SourceTagFilter = SourceContainer;
    }
    if (bHasTarget)
    {
        GASAuthoring_ParseTags(*TargetTagsArray, TargetContainer, TargetResolved, TargetUnknown);
        AttrPtr->TargetTagFilter = TargetContainer;
    }

    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Modifier.Blueprint, Edit))
    {
        return SaveFailed;
    }

    // Read back both filters via the same reflected sub-struct. Each provided filter must round-trip
    // exactly to the parsed container (FGameplayTagContainer::operator== is GAMEPLAYTAGS_API).
    TArray<TSharedPtr<FJsonValue>> SourceFilterReadback, TargetFilterReadback;
    bool bSourceOk = !bHasSource;
    bool bTargetOk = !bHasTarget;
    if (const FAttributeBasedFloat* RB = GetMagnitudeSubStructPtr<FAttributeBasedFloat>(Mag, TEXT("AttributeBasedMagnitude")))
    {
        SourceFilterReadback = mcp_common::GameplayTagsToJson(RB->SourceTagFilter);
        TargetFilterReadback = mcp_common::GameplayTagsToJson(RB->TargetTagFilter);
        if (bHasSource)
        {
            bSourceOk = RB->SourceTagFilter == SourceContainer;
        }
        if (bHasTarget)
        {
            bTargetOk = RB->TargetTagFilter == TargetContainer;
        }
    }

    TSharedPtr<FJsonObject> Result = GASAuthoring_ModifierResult(Modifier, TEXT("AttributeBased"));
    Result->SetBoolField(TEXT("source_tags_set"), bHasSource);
    Result->SetBoolField(TEXT("target_tags_set"), bHasTarget);
    Result->SetArrayField(TEXT("source_resolved_tags"), SourceResolved);
    Result->SetArrayField(TEXT("source_unknown_tags"), SourceUnknown);
    Result->SetArrayField(TEXT("target_resolved_tags"), TargetResolved);
    Result->SetArrayField(TEXT("target_unknown_tags"), TargetUnknown);
    Result->SetArrayField(TEXT("source_tag_filter"), SourceFilterReadback);
    Result->SetArrayField(TEXT("target_tag_filter"), TargetFilterReadback);
    Result->SetBoolField(TEXT("set"), bSourceOk && bTargetOk);
    return mcp_common::MakeOkReply(Result);
}

// GameplayAbility CDO authoring

// MCP:set_ability_cost - Set the Cost GameplayEffect class on a GameplayAbility CDO (the GE applied when the ability is committed). A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param ability_blueprint_path - Full asset path of the GameplayAbility Blueprint
//   @param cost_effect_path - Full asset path of the GameplayEffect Blueprint to use as the cost
//   @returns { ability_blueprint_path: string, cost_effect_path: string, cost_effect_class: string, cost_effect_class_matches: bool, set: bool }
TSharedPtr<FJsonObject> HandleSetAbilityCost(const TSharedPtr<FJsonObject>& Params)
{
    return SetAbilityGameplayEffectClass(Params, TEXT("CostGameplayEffectClass"), TEXT("cost_effect_path"), TEXT("cost_effect_class"));
}

// MCP:set_ability_cooldown - Set the Cooldown GameplayEffect class on a GameplayAbility CDO (gates re-use until it expires). A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param ability_blueprint_path - Full asset path of the GameplayAbility Blueprint
//   @param cooldown_effect_path - Full asset path of the GameplayEffect Blueprint to use as the cooldown (typically HasDuration)
//   @returns { ability_blueprint_path: string, cooldown_effect_path: string, cooldown_effect_class: string, cooldown_effect_class_matches: bool, set: bool }
TSharedPtr<FJsonObject> HandleSetAbilityCooldown(const TSharedPtr<FJsonObject>& Params)
{
    return SetAbilityGameplayEffectClass(Params, TEXT("CooldownGameplayEffectClass"), TEXT("cooldown_effect_path"), TEXT("cooldown_effect_class"));
}

// MCP:configure_ability_tags - Set a GameplayAbility CDO's asset tags from a list of tag strings. A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param ability_blueprint_path - Full asset path of the GA Blueprint
//   @param asset_tags - List of gameplay tag strings, e.g. [Ability.Fire]
//   @returns { ability_blueprint_path: string, resolved_tags: [string], unknown_tags: [string], ability_tags: [string], tag_count: int, configured: bool }
TSharedPtr<FJsonObject> HandleConfigureAbilityTags(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ability_blueprint_path"), BlueprintPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const TArray<TSharedPtr<FJsonValue>>* AssetTagsArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("asset_tags"), AssetTagsArray) || !AssetTagsArray)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: asset_tags (array of strings)"));
    }

    UBlueprint* Blueprint = nullptr;
    FString Error;
    UGameplayAbility* AbilityCDO = GASAuthoring_LoadBlueprintCDO<UGameplayAbility>(BlueprintPath, TEXT("GameplayAbility"), Blueprint, Error);
    if (!AbilityCDO)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    if (TSharedPtr<FJsonObject> Refused = GASAuthoring_RefuseUnsavableEdit(Blueprint))
    {
        return Refused;
    }

    // Resolve each string into a registered FGameplayTag (must exist in the tag dictionary).
    FGameplayTagContainer NewContainer;
    TArray<TSharedPtr<FJsonValue>> ResolvedTags;
    TArray<TSharedPtr<FJsonValue>> UnknownTags;
    GASAuthoring_ParseTags(*AssetTagsArray, NewContainer, ResolvedTags, UnknownTags);

    FGASAuthoringCDOEdit Edit(AbilityCDO);
    AbilityCDO->Modify();

    // EditorGetAssetTags() returns a mutable ref to the ability's tag container
    // (WITH_EDITOR only; we are an editor plugin). This is the supported way to
    // mutate the non-public AbilityTags property. Verified in GameplayAbility.cpp.
    AbilityCDO->EditorGetAssetTags() = NewContainer;

    // Persist the CDO edit without recompiling (AbilityTags is a native UProperty on
    // the UGameplayAbility parent; see set_gameplay_effect_duration for rationale).
    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Blueprint, Edit))
    {
        return SaveFailed;
    }

    // Read back from the same CDO via the public const accessor GetAssetTags().
    UGameplayAbility* ReadCDO = AbilityCDO;
    const FGameplayTagContainer& ReadTags = ReadCDO ? ReadCDO->GetAssetTags() : NewContainer;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("ability_blueprint_path"), BlueprintPath);
    Result->SetArrayField(TEXT("resolved_tags"), ResolvedTags);
    Result->SetArrayField(TEXT("unknown_tags"), UnknownTags);
    Result->SetArrayField(TEXT("ability_tags"), mcp_common::GameplayTagsToJson(ReadTags));
    Result->SetNumberField(TEXT("tag_count"), ReadTags.Num());
    Result->SetBoolField(TEXT("configured"), true);
    return mcp_common::MakeOkReply(Result);
}

// ===========================================================================
//  GameplayCueNotify + GameplayEffect component authoring
// ===========================================================================

// GameplayCueNotify shared helpers ----------------------------------------------------
namespace
{
    // Sets GameplayCueTag, and GameplayCueName to the tag's name, on a UGameplayCueNotify_Static or
    // AGameplayCueNotify_Actor CDO.
    void GASAuthoring_SetCueNotifyTag(UObject* CueCDO, const FGameplayTag& Tag)
    {
        if (UGameplayCueNotify_Static* StaticCue = Cast<UGameplayCueNotify_Static>(CueCDO))
        {
            StaticCue->GameplayCueTag = Tag;
            StaticCue->GameplayCueName = Tag.GetTagName();
        }
        else if (AGameplayCueNotify_Actor* ActorCue = Cast<AGameplayCueNotify_Actor>(CueCDO))
        {
            ActorCue->GameplayCueTag = Tag;
            ActorCue->GameplayCueName = Tag.GetTagName();
        }
    }

    // create_gameplay_cue_notify_*: sets the optional gameplay_cue_tag on the new notify's CDO and saves the
    // Blueprint. The tag is resolved against the registered tag dictionary (ErrorIfNotFound=false: a project
    // may not have authored GameplayCue.* tags, so an unknown one is reported, not an error). Adds
    // cue_tag_set, cue_tag_requested and cue_tag to Result. The error reply when the save fails.
    TSharedPtr<FJsonObject> GASAuthoring_SetCreatedCueTag(const TSharedPtr<FJsonObject>& Params, const FGASAuthoringCreatedBlueprint& Created,
                                                          const TSharedPtr<FJsonObject>& Result)
    {
        bool bCueTagSet = false;
        FString CueTagApplied;
        FString CueTagStr;
        if (Params->TryGetStringField(TEXT("gameplay_cue_tag"), CueTagStr) && !CueTagStr.IsEmpty())
        {
            UBlueprint* Blueprint = Created.Blueprint;
            UObject* CueCDO = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
            if (CueCDO && CueCDO->IsA(Created.ParentClass))
            {
                const FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(FName(*CueTagStr), /*ErrorIfNotFound=*/false);
                if (Tag.IsValid())
                {
                    if (TSharedPtr<FJsonObject> Refused = GASAuthoring_RefuseUnsavableEdit(Blueprint))
                    {
                        return Refused;
                    }
                    FGASAuthoringCDOEdit Edit(CueCDO);
                    CueCDO->Modify();
                    GASAuthoring_SetCueNotifyTag(CueCDO, Tag);
                    bCueTagSet = true;
                    CueTagApplied = Tag.ToString();

                    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Blueprint, Edit))
                    {
                        return SaveFailed;
                    }
                }
            }
        }

        Result->SetBoolField(TEXT("cue_tag_set"), bCueTagSet);
        if (!CueTagStr.IsEmpty())
        {
            Result->SetStringField(TEXT("cue_tag_requested"), CueTagStr);
        }
        if (bCueTagSet)
        {
            Result->SetStringField(TEXT("cue_tag"), CueTagApplied);
        }
        return nullptr;
    }

    // Reads the required cue_notify_path and loads that GameplayCueNotify Blueprint. Null with OutError
    // when the parameter is missing, the asset does not load or it has no GeneratedClass.
    UBlueprint* GASAuthoring_ReadCueNotifyBlueprint(const TSharedPtr<FJsonObject>& Params, FString& OutBlueprintPath, FString& OutError)
    {
        if (!Params->TryGetStringField(TEXT("cue_notify_path"), OutBlueprintPath))
        {
            OutError = TEXT("Missing required parameter: cue_notify_path");
            return nullptr;
        }
        UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *OutBlueprintPath);
        if (!Blueprint)
        {
            OutError = FString::Printf(TEXT("GameplayCueNotify Blueprint not found: %s"), *OutBlueprintPath);
            return nullptr;
        }
        if (!Blueprint->GeneratedClass)
        {
            OutError = FString::Printf(TEXT("Blueprint has no GeneratedClass: %s"), *OutBlueprintPath);
            return nullptr;
        }
        return Blueprint;
    }

    // Applies the optional gameplay_cue_tag of a set_gameplay_cue_notify_*_properties command to CueCDO and
    // counts it in InOutPropertiesSet. The tag must be registered: an unknown one is the error reply, and the
    // CDO is left unwritten.
    TSharedPtr<FJsonObject> GASAuthoring_ApplyCueTagParam(const TSharedPtr<FJsonObject>& Params, UObject* CueCDO, int32& InOutPropertiesSet)
    {
        FString CueTagStr;
        if (Params->TryGetStringField(TEXT("gameplay_cue_tag"), CueTagStr) && !CueTagStr.IsEmpty())
        {
            const FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(FName(*CueTagStr), /*ErrorIfNotFound=*/false);
            if (!Tag.IsValid())
            {
                return mcp_common::MakeErrorReply(FString::Printf(TEXT("GameplayCue tag is not registered: %s"), *CueTagStr));
            }
            GASAuthoring_SetCueNotifyTag(CueCDO, Tag);
            ++InOutPropertiesSet;
        }
        return nullptr;
    }

    // Applies the lifetime/instancing/override props present in Params to an AGameplayCueNotify_Actor CDO
    // (public UPROPERTYs on AGameplayCueNotify_Actor, so direct member writes) and returns how many it set.
    int32 GASAuthoring_ApplyCueActorProperties(const TSharedPtr<FJsonObject>& Params, AGameplayCueNotify_Actor* CueCDO)
    {
        int32 PropertiesSet = 0;
        { bool b = false; if (Params->TryGetBoolField(TEXT("auto_destroy_on_remove"), b)) { CueCDO->bAutoDestroyOnRemove = b; ++PropertiesSet; } }
        { double d = 0.0; if (Params->TryGetNumberField(TEXT("auto_destroy_delay"), d)) { CueCDO->AutoDestroyDelay = (float)d; ++PropertiesSet; } }
        { bool b = false; if (Params->TryGetBoolField(TEXT("auto_attach_to_owner"), b)) { CueCDO->bAutoAttachToOwner = b; ++PropertiesSet; } }
        { bool b = false; if (Params->TryGetBoolField(TEXT("is_override"), b)) { CueCDO->IsOverride = b; ++PropertiesSet; } }
        { bool b = false; if (Params->TryGetBoolField(TEXT("unique_instance_per_instigator"), b)) { CueCDO->bUniqueInstancePerInstigator = b; ++PropertiesSet; } }
        { bool b = false; if (Params->TryGetBoolField(TEXT("unique_instance_per_source_object"), b)) { CueCDO->bUniqueInstancePerSourceObject = b; ++PropertiesSet; } }
        { bool b = false; if (Params->TryGetBoolField(TEXT("allow_multiple_on_active_events"), b)) { CueCDO->bAllowMultipleOnActiveEvents = b; ++PropertiesSet; } }
        { bool b = false; if (Params->TryGetBoolField(TEXT("allow_multiple_while_active_events"), b)) { CueCDO->bAllowMultipleWhileActiveEvents = b; ++PropertiesSet; } }
        { double d = 0.0; if (Params->TryGetNumberField(TEXT("num_preallocated_instances"), d)) { CueCDO->NumPreallocatedInstances = (int32)d; ++PropertiesSet; } }
        return PropertiesSet;
    }

    // The lifetime/instancing/override props of an AGameplayCueNotify_Actor CDO, read back into Result.
    void GASAuthoring_AddCueActorPropertyFields(const TSharedPtr<FJsonObject>& Result, const AGameplayCueNotify_Actor* CueCDO)
    {
        Result->SetBoolField(TEXT("auto_destroy_on_remove"), CueCDO->bAutoDestroyOnRemove);
        Result->SetNumberField(TEXT("auto_destroy_delay"), CueCDO->AutoDestroyDelay);
        Result->SetBoolField(TEXT("auto_attach_to_owner"), CueCDO->bAutoAttachToOwner);
        Result->SetBoolField(TEXT("is_override"), CueCDO->IsOverride);
        Result->SetBoolField(TEXT("unique_instance_per_instigator"), CueCDO->bUniqueInstancePerInstigator);
        Result->SetBoolField(TEXT("unique_instance_per_source_object"), CueCDO->bUniqueInstancePerSourceObject);
        Result->SetBoolField(TEXT("allow_multiple_on_active_events"), CueCDO->bAllowMultipleOnActiveEvents);
        Result->SetBoolField(TEXT("allow_multiple_while_active_events"), CueCDO->bAllowMultipleWhileActiveEvents);
        Result->SetNumberField(TEXT("num_preallocated_instances"), CueCDO->NumPreallocatedInstances);
    }

    // The cue notify a set_gameplay_cue_notify_static/burst_properties command edits, how many properties it set, and
    // the snapshot of its CDO taken before the first write (undone on an error, kept once the edit is saved).
    struct FGASAuthoringCueEdit
    {
        FString BlueprintPath;
        UBlueprint* Blueprint = nullptr;
        UGameplayCueNotify_Static* CueCDO = nullptr;
        int32 PropertiesSet = 0;
        TOptional<FGASAuthoringCDOEdit> CDOEdit;
    };

    // set_gameplay_cue_notify_static/burst_properties: loads the cue_notify_path Blueprint, whose CDO must be a CueClass
    // (CueKind names it in the error), and applies the gameplay_cue_tag and is_override given (public UPROPERTYs on
    // UGameplayCueNotify_Static, written directly after Modify(), so a re-run is idempotent). The error reply when a
    // step fails or neither property is given; Out.CDOEdit then puts the CDO back as it was.
    TSharedPtr<FJsonObject> GASAuthoring_ApplyCueStaticProperties(const TSharedPtr<FJsonObject>& Params, const UClass* CueClass,
                                                                  const TCHAR* CueKind, FGASAuthoringCueEdit& Out)
    {
        FString Error;
        Out.Blueprint = GASAuthoring_ReadCueNotifyBlueprint(Params, Out.BlueprintPath, Error);
        if (!Out.Blueprint)
        {
            return mcp_common::MakeErrorReply(Error);
        }
        Out.CueCDO = Cast<UGameplayCueNotify_Static>(Out.Blueprint->GeneratedClass->GetDefaultObject());
        if (!Out.CueCDO || !Out.CueCDO->IsA(CueClass))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Blueprint is not %s (or subclass): %s"), CueKind, *Out.BlueprintPath));
        }
        if (TSharedPtr<FJsonObject> Refused = GASAuthoring_RefuseUnsavableEdit(Out.Blueprint))
        {
            return Refused;
        }

        Out.CDOEdit.Emplace(Out.CueCDO);
        Out.CueCDO->Modify();

        if (TSharedPtr<FJsonObject> TagError = GASAuthoring_ApplyCueTagParam(Params, Out.CueCDO, Out.PropertiesSet))
        {
            return TagError;
        }
        bool bIsOverride = false;
        if (Params->TryGetBoolField(TEXT("is_override"), bIsOverride))
        {
            Out.CueCDO->IsOverride = bIsOverride;
            ++Out.PropertiesSet;
        }

        if (Out.PropertiesSet == 0)
        {
            return mcp_common::MakeErrorReply(
                TEXT("No properties provided (set at least one of gameplay_cue_tag|is_override)"));
        }
        return nullptr;
    }

    // cue_notify_path, properties_set, and the GameplayCueTag, GameplayCueName and IsOverride read back from the CDO.
    TSharedPtr<FJsonObject> GASAuthoring_CueStaticResult(const FGASAuthoringCueEdit& Edit)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("cue_notify_path"), Edit.BlueprintPath);
        Result->SetNumberField(TEXT("properties_set"), Edit.PropertiesSet);
        Result->SetStringField(TEXT("gameplay_cue_tag"), Edit.CueCDO->GameplayCueTag.ToString());
        Result->SetStringField(TEXT("gameplay_cue_name"), Edit.CueCDO->GameplayCueName.ToString());
        Result->SetBoolField(TEXT("is_override"), Edit.CueCDO->IsOverride);
        return Result;
    }
}

// GameplayCueNotify authoring

// MCP:create_gameplay_cue_notify_static - Create a Blueprint parented to UGameplayCueNotify_Static (non-instanced, one-shot) at package_path/name, compile and save it, or reuse the one of that class already there (created false, already_existed true); a Blueprint of another class there is an error. A registered gameplay_cue_tag is written to the CDO and saved: a read-only root or a .uasset read-only on disk refuses that write and a failed save undoes it; an unregistered tag only yields cue_tag_set=false. Refused during PIE.
//   @param name - Asset name without a path, e.g. "GCN_Hit".
//   @param package_path - Content folder for the new asset, e.g. "/Game/GAS/Cues"; defaults to /Game/Blueprints.
//   @param gameplay_cue_tag - Registered gameplay tag written as GameplayCueTag and GameplayCueName, e.g. "GameplayCue.Hit"; an unregistered tag is skipped (cue_tag_set=false), not an error.
//   @returns { name: string, asset_path: string, parent_class: string, generated_class: string, is_gameplay_cue_notify: bool, cue_tag_set: bool, cue_tag_requested: string, cue_tag: string, already_existed: bool, created: bool }
TSharedPtr<FJsonObject> HandleCreateGameplayCueNotifyStatic(const TSharedPtr<FJsonObject>& Params)
{
    FGASAuthoringCreatedBlueprint Created;
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_CreateBlueprint(Params, UGameplayCueNotify_Static::StaticClass(), Created))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Result = GASAuthoring_CreatedBlueprintFields(Created, TEXT("GameplayCueNotify_Static"), TEXT("is_gameplay_cue_notify"));
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_SetCreatedCueTag(Params, Created, Result))
    {
        return Error;
    }
    Result->SetBoolField(TEXT("already_existed"), Created.bAlreadyExisted);
    Result->SetBoolField(TEXT("created"), !Created.bAlreadyExisted);
    return mcp_common::MakeOkReply(Result);
}

// MCP:create_gameplay_cue_notify_actor - Create a Blueprint parented to AGameplayCueNotify_Actor (a spawned, instanced cue actor) at package_path/name, compile and save it, or reuse the one of that class already there (created false, already_existed true); a Blueprint of another class there is an error. A registered gameplay_cue_tag is written to the CDO and saved: a read-only root or a .uasset read-only on disk refuses that write and a failed save undoes it; an unregistered tag only yields cue_tag_set=false. Refused during PIE.
//   @param name - Asset name without a path, e.g. "GCN_Beam".
//   @param package_path - Content folder for the new asset, e.g. "/Game/GAS/Cues"; defaults to /Game/Blueprints.
//   @param gameplay_cue_tag - Registered gameplay tag written as GameplayCueTag and GameplayCueName, e.g. "GameplayCue.Beam"; an unregistered tag is skipped (cue_tag_set=false), not an error.
//   @returns { name: string, asset_path: string, parent_class: string, generated_class: string, is_gameplay_cue_notify: bool, cue_tag_set: bool, cue_tag_requested: string, cue_tag: string, already_existed: bool, created: bool }
TSharedPtr<FJsonObject> HandleCreateGameplayCueNotifyActor(const TSharedPtr<FJsonObject>& Params)
{
    FGASAuthoringCreatedBlueprint Created;
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_CreateBlueprint(Params, AGameplayCueNotify_Actor::StaticClass(), Created))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Result = GASAuthoring_CreatedBlueprintFields(Created, TEXT("GameplayCueNotify_Actor"), TEXT("is_gameplay_cue_notify"));
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_SetCreatedCueTag(Params, Created, Result))
    {
        return Error;
    }
    Result->SetBoolField(TEXT("already_existed"), Created.bAlreadyExisted);
    Result->SetBoolField(TEXT("created"), !Created.bAlreadyExisted);
    return mcp_common::MakeOkReply(Result);
}

// MCP:create_gameplay_cue_notify_burst - Create a Blueprint parented to UGameplayCueNotify_Burst (modern non-instanced one-shot notify), compile and save it, or reuse the one of that class already there (created false, already_existed true); a Blueprint of another class there is an error. A registered gameplay_cue_tag is written to the CDO and saved: a read-only root or a .uasset read-only on disk refuses that write and a failed save undoes it.
//   @param name - Asset name, e.g. GCN_Hit
//   @param package_path - Target content path, defaults to /Game/Blueprints
//   @param gameplay_cue_tag - Optional registered GameplayCue tag to set on the CDO
//   @returns { name: string, asset_path: string, parent_class: string, generated_class: string, is_gameplay_cue_notify: bool, cue_tag_set: bool, cue_tag_requested: string, cue_tag: string, already_existed: bool, created: bool }
TSharedPtr<FJsonObject> HandleCreateGameplayCueNotifyBurst(const TSharedPtr<FJsonObject>& Params)
{
    FGASAuthoringCreatedBlueprint Created;
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_CreateBlueprint(Params, UGameplayCueNotify_Burst::StaticClass(), Created))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Result = GASAuthoring_CreatedBlueprintFields(Created, TEXT("GameplayCueNotify_Burst"), TEXT("is_gameplay_cue_notify"));
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_SetCreatedCueTag(Params, Created, Result))
    {
        return Error;
    }
    Result->SetBoolField(TEXT("already_existed"), Created.bAlreadyExisted);
    Result->SetBoolField(TEXT("created"), !Created.bAlreadyExisted);
    return mcp_common::MakeOkReply(Result);
}

// Advanced GameplayCue: modern looping class + deep config of the actor notify CDO.
// APIs (GameplayAbilities plugin):
//   AGameplayCueNotify_Looping : AGameplayCueNotify_Actor (Blueprintable, MinimalAPI)  GameplayCueNotify_Looping.h
//   AGameplayCueNotify_Actor public UPROPERTYs: bAutoDestroyOnRemove, AutoDestroyDelay, bAutoAttachToOwner,
//     IsOverride, bUniqueInstancePerInstigator/SourceObject, bAllowMultipleOnActive/WhileActiveEvents,
//     NumPreallocatedInstances  (GameplayCueNotify_Actor.h, all public data members -> no reflection)

// MCP:create_gameplay_cue_notify_looping - Create a Blueprint parented to AGameplayCueNotify_Looping (modern instanced looping notify), compile and save it, or reuse the one of that class already there (created false, already_existed true); a Blueprint of another class there is an error. A registered gameplay_cue_tag is written to the CDO and saved: a read-only root or a .uasset read-only on disk refuses that write and a failed save undoes it.
//   @param name - Asset name, e.g. GCN_Aura
//   @param package_path - Target content path, defaults to /Game/Blueprints
//   @param gameplay_cue_tag - Optional registered GameplayCue tag to set on the CDO
//   @returns { name: string, asset_path: string, parent_class: string, generated_class: string, is_gameplay_cue_notify: bool, cue_tag_set: bool, cue_tag_requested: string, cue_tag: string, already_existed: bool, created: bool }
TSharedPtr<FJsonObject> HandleCreateGameplayCueNotifyLooping(const TSharedPtr<FJsonObject>& Params)
{
    FGASAuthoringCreatedBlueprint Created;
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_CreateBlueprint(Params, AGameplayCueNotify_Looping::StaticClass(), Created))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Result = GASAuthoring_CreatedBlueprintFields(Created, TEXT("GameplayCueNotify_Looping"), TEXT("is_gameplay_cue_notify"));
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_SetCreatedCueTag(Params, Created, Result))
    {
        return Error;
    }
    Result->SetBoolField(TEXT("already_existed"), Created.bAlreadyExisted);
    Result->SetBoolField(TEXT("created"), !Created.bAlreadyExisted);
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_gameplay_cue_notify_actor_properties - Write the lifetime, instancing and override properties you pass on the class default object of an AGameplayCueNotify_Actor Blueprint (Looping ones included), then save it without recompiling and without an undo entry. Properties left out keep their values; passing none, or a Blueprint of another class, is an error. A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit. It never sets the cue tag: set_gameplay_cue_notify_looping_properties does for Looping notifies, set_gameplay_cue_notify_static_properties and set_gameplay_cue_notify_burst_properties for non-actor ones. get_gameplay_cue_notify_info reads them back.
//   @param cue_notify_path - Full asset path of the GameplayCueNotify_Actor/_Looping Blueprint
//   @param auto_destroy_on_remove - Optional bool: auto recycle/destroy when OnRemove fires
//   @param auto_destroy_delay - Optional float: seconds to stay alive before auto-destroy (needs auto_destroy_on_remove)
//   @param auto_attach_to_owner - Optional bool: attach the cue actor to the target while active
//   @param is_override - Optional bool: this cue overrides parent-tag cues instead of running in addition
//   @param unique_instance_per_instigator - Optional bool: a new instance per instigator
//   @param unique_instance_per_source_object - Optional bool: a new instance per source object
//   @param allow_multiple_on_active_events - Optional bool: re-trigger On Burst if already triggered
//   @param allow_multiple_while_active_events - Optional bool: re-trigger On Become Relevant if already triggered
//   @param num_preallocated_instances - Optional int: how many instances of the cue to preallocate
//   @returns { cue_notify_path: string, properties_set: int, auto_destroy_on_remove: bool, auto_destroy_delay: number, auto_attach_to_owner: bool, is_override: bool, unique_instance_per_instigator: bool, unique_instance_per_source_object: bool, allow_multiple_on_active_events: bool, allow_multiple_while_active_events: bool, num_preallocated_instances: int, set: bool }
TSharedPtr<FJsonObject> HandleSetGameplayCueNotifyActorProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintPath;
    FString Error;
    UBlueprint* Blueprint = GASAuthoring_ReadCueNotifyBlueprint(Params, BlueprintPath, Error);
    if (!Blueprint)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    AGameplayCueNotify_Actor* CueCDO = Cast<AGameplayCueNotify_Actor>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!CueCDO)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Blueprint is not an AGameplayCueNotify_Actor (or subclass): %s"), *BlueprintPath));
    }
    if (TSharedPtr<FJsonObject> Refused = GASAuthoring_RefuseUnsavableEdit(Blueprint))
    {
        return Refused;
    }

    FGASAuthoringCDOEdit Edit(CueCDO);
    CueCDO->Modify();

    // Apply only the properties that were provided (set-to-fixed-value -> idempotent across runs).
    const int32 PropertiesSet = GASAuthoring_ApplyCueActorProperties(Params, CueCDO);

    if (PropertiesSet == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No properties provided (set at least one of auto_destroy_on_remove|auto_destroy_delay|auto_attach_to_owner|is_override|unique_instance_per_instigator|unique_instance_per_source_object|allow_multiple_on_active_events|allow_multiple_while_active_events|num_preallocated_instances)"));
    }

    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Blueprint, Edit))
    {
        return SaveFailed;
    }

    // Read back from the CDO (no recompile — native UProperties serialize with the package).
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("cue_notify_path"), BlueprintPath);
    Result->SetNumberField(TEXT("properties_set"), PropertiesSet);
    GASAuthoring_AddCueActorPropertyFields(Result, CueCDO);
    Result->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeOkReply(Result);
}

// GameplayCueNotify introspection + per-variant tag/override setters.
// get_gameplay_cue_notify_info reads the CDO of any GCN variant (Static/Burst/Actor/Looping), classifying
// notify_type by a Cast<> cascade (Burst before Static, Looping before Actor) and reporting the public tag +
// the Actor/Looping lifetime UPROPERTYs. The setters follow the same Modify + RequestGameplayTag +
// SaveAssetPackage + read-back pattern as set_gameplay_cue_notify_actor_properties, for the Static/Burst
// (GameplayCueTag + IsOverride, both public on UGameplayCueNotify_Static) and Looping (Actor lifetime props)
// variants. BurstEffects/LoopingEffects/ApplicationEffects are protected UObject-ref structs: they are never
// touched, and the reply says so in a note. APIs (GameplayAbilities):
//   UGameplayCueNotify_Static: GameplayCueTag/GameplayCueName/IsOverride (public; GameplayCueNotify_Static.h)
//   AGameplayCueNotify_Actor public lifetime props (GameplayCueNotify_Actor.h)

// MCP:get_gameplay_cue_notify_info - Read a GameplayCueNotify Blueprint CDO: variant, tag and (Actor/Looping) lifetime props
//   @param cue_notify_path - Full asset path of the GameplayCueNotify Blueprint (Static/Burst/Actor/Looping)
//   @returns { cue_notify_path: string, parent_class: string, notify_type: string, gameplay_cue_tag: string, gameplay_cue_name: string, is_override: bool, auto_destroy_on_remove: bool, auto_destroy_delay: number, auto_attach_to_owner: bool, unique_instance_per_instigator: bool, unique_instance_per_source_object: bool, allow_multiple_on_active_events: bool, allow_multiple_while_active_events: bool, num_preallocated_instances: int }
TSharedPtr<FJsonObject> HandleGetGameplayCueNotifyInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintPath;
    FString Error;
    UBlueprint* Blueprint = GASAuthoring_ReadCueNotifyBlueprint(Params, BlueprintPath, Error);
    if (!Blueprint)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("cue_notify_path"), BlueprintPath);
    Result->SetStringField(TEXT("parent_class"),
        Blueprint->ParentClass ? Blueprint->ParentClass->GetName() : FString());

    // Burst derives from Static and Looping from Actor; the tag lives on the CDO either way.
    FString NotifyType;
    FString CueTag;
    FString CueName;
    if (UGameplayCueNotify_Static* Static = Cast<UGameplayCueNotify_Static>(CDO))
    {
        NotifyType = Static->IsA<UGameplayCueNotify_Burst>() ? TEXT("Burst") : TEXT("Static");
        CueTag = Static->GameplayCueTag.ToString();
        CueName = Static->GameplayCueName.ToString();
        Result->SetBoolField(TEXT("is_override"), Static->IsOverride);
    }
    else if (AGameplayCueNotify_Actor* Actor = Cast<AGameplayCueNotify_Actor>(CDO))
    {
        NotifyType = Actor->IsA<AGameplayCueNotify_Looping>() ? TEXT("Looping") : TEXT("Actor");
        CueTag = Actor->GameplayCueTag.ToString();
        CueName = Actor->GameplayCueName.ToString();
        Result->SetBoolField(TEXT("is_override"), Actor->IsOverride);
        Result->SetBoolField(TEXT("auto_destroy_on_remove"), Actor->bAutoDestroyOnRemove);
        Result->SetNumberField(TEXT("auto_destroy_delay"), Actor->AutoDestroyDelay);
        Result->SetBoolField(TEXT("auto_attach_to_owner"), Actor->bAutoAttachToOwner);
        Result->SetBoolField(TEXT("unique_instance_per_instigator"), Actor->bUniqueInstancePerInstigator);
        Result->SetBoolField(TEXT("unique_instance_per_source_object"), Actor->bUniqueInstancePerSourceObject);
        Result->SetBoolField(TEXT("allow_multiple_on_active_events"), Actor->bAllowMultipleOnActiveEvents);
        Result->SetBoolField(TEXT("allow_multiple_while_active_events"), Actor->bAllowMultipleWhileActiveEvents);
        Result->SetNumberField(TEXT("num_preallocated_instances"), Actor->NumPreallocatedInstances);
    }
    else
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Blueprint is not a GameplayCueNotify (Static/Burst/Actor/Looping): %s"), *BlueprintPath));
    }

    Result->SetStringField(TEXT("notify_type"), NotifyType);
    Result->SetStringField(TEXT("gameplay_cue_tag"), CueTag);
    Result->SetStringField(TEXT("gameplay_cue_name"), CueName);
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_gameplay_cue_notify_static_properties - Set GameplayCueTag (mirrored into GameplayCueName) and/or IsOverride on the CDO of a UGameplayCueNotify_Static Blueprint, then save it without recompiling. Needs at least one of the two; an unregistered tag fails before anything is written. Returns properties_set and the values read back. A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param cue_notify_path - Asset path of the GameplayCueNotify_Static Blueprint, e.g. "/Game/GAS/Cues/GCN_Hit".
//   @param gameplay_cue_tag - Optional registered GameplayCue tag (sets GameplayCueTag + mirrors GameplayCueName); unregistered -> clean error
//   @param is_override - Optional bool: this cue overrides parent-tag cues instead of running in addition
//   @returns { cue_notify_path: string, properties_set: int, gameplay_cue_tag: string, gameplay_cue_name: string, is_override: bool, set: bool }
TSharedPtr<FJsonObject> HandleSetGameplayCueNotifyStaticProperties(const TSharedPtr<FJsonObject>& Params)
{
    FGASAuthoringCueEdit Edit;
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_ApplyCueStaticProperties(Params, UGameplayCueNotify_Static::StaticClass(),
                                                                              TEXT("a UGameplayCueNotify_Static"), Edit))
    {
        return Error;
    }
    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Edit.Blueprint, *Edit.CDOEdit))
    {
        return SaveFailed;
    }

    TSharedPtr<FJsonObject> Result = GASAuthoring_CueStaticResult(Edit);
    Result->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_gameplay_cue_notify_burst_properties - Set GameplayCueTag (mirrored into GameplayCueName) and/or IsOverride on the CDO of a UGameplayCueNotify_Burst Blueprint, then save it without recompiling. Needs at least one; an unregistered tag fails before anything is written. Burst effects (particles, sounds) are not authored. Returns the values read back. A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param cue_notify_path - Asset path of the GameplayCueNotify_Burst Blueprint, e.g. "/Game/GAS/Cues/GCN_Hit".
//   @param gameplay_cue_tag - Optional registered GameplayCue tag (sets GameplayCueTag + mirrors GameplayCueName); unregistered -> clean error
//   @param is_override - Optional bool: this cue overrides parent-tag cues instead of running in addition
//   @returns { cue_notify_path: string, properties_set: int, gameplay_cue_tag: string, gameplay_cue_name: string, is_override: bool, note: string, set: bool }
TSharedPtr<FJsonObject> HandleSetGameplayCueNotifyBurstProperties(const TSharedPtr<FJsonObject>& Params)
{
    // Burst only exposes the public Static-inherited GameplayCueTag/IsOverride. Its
    // BurstEffects/DefaultSpawnCondition/DefaultPlacementInfo are protected UObject-ref
    // (Niagara/Sound) structs -> asset-ref authoring deferred (see note below).
    FGASAuthoringCueEdit Edit;
    if (TSharedPtr<FJsonObject> Error = GASAuthoring_ApplyCueStaticProperties(Params, UGameplayCueNotify_Burst::StaticClass(),
                                                                              TEXT("a UGameplayCueNotify_Burst"), Edit))
    {
        return Error;
    }
    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Edit.Blueprint, *Edit.CDOEdit))
    {
        return SaveFailed;
    }

    TSharedPtr<FJsonObject> Result = GASAuthoring_CueStaticResult(Edit);
    Result->SetStringField(TEXT("note"), TEXT("burst effects (particles/sounds) require asset-ref authoring; deferred"));
    Result->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_gameplay_cue_notify_looping_properties - Set GameplayCueTag + Actor lifetime/instancing/override props on an AGameplayCueNotify_Looping Blueprint CDO. A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param cue_notify_path - Full asset path of the GameplayCueNotify_Looping Blueprint
//   @param gameplay_cue_tag - Optional registered GameplayCue tag (sets GameplayCueTag + mirrors GameplayCueName); unregistered -> clean error
//   @param auto_destroy_on_remove - Optional bool: auto recycle/destroy when OnRemove fires
//   @param auto_destroy_delay - Optional float: seconds to stay alive before auto-destroy (needs auto_destroy_on_remove)
//   @param auto_attach_to_owner - Optional bool: attach the cue actor to the target while active
//   @param is_override - Optional bool: this cue overrides parent-tag cues instead of running in addition
//   @param unique_instance_per_instigator - Optional bool: a new instance per instigator
//   @param unique_instance_per_source_object - Optional bool: a new instance per source object
//   @param allow_multiple_on_active_events - Optional bool: re-trigger On Burst if already triggered
//   @param allow_multiple_while_active_events - Optional bool: re-trigger On Become Relevant if already triggered
//   @param num_preallocated_instances - Optional int: how many instances of the cue to preallocate
//   @returns { cue_notify_path: string, properties_set: int, gameplay_cue_tag: string, gameplay_cue_name: string, auto_destroy_on_remove: bool, auto_destroy_delay: number, auto_attach_to_owner: bool, is_override: bool, unique_instance_per_instigator: bool, unique_instance_per_source_object: bool, allow_multiple_on_active_events: bool, allow_multiple_while_active_events: bool, num_preallocated_instances: int, note: string, set: bool }
TSharedPtr<FJsonObject> HandleSetGameplayCueNotifyLoopingProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintPath;
    FString Error;
    UBlueprint* Blueprint = GASAuthoring_ReadCueNotifyBlueprint(Params, BlueprintPath, Error);
    if (!Blueprint)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    AGameplayCueNotify_Looping* CueCDO = Cast<AGameplayCueNotify_Looping>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!CueCDO)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Blueprint is not an AGameplayCueNotify_Looping (or subclass): %s"), *BlueprintPath));
    }
    if (TSharedPtr<FJsonObject> Refused = GASAuthoring_RefuseUnsavableEdit(Blueprint))
    {
        return Refused;
    }

    FGASAuthoringCDOEdit Edit(CueCDO);
    CueCDO->Modify();

    // GameplayCueTag + the public Actor-inherited lifetime props (same set as
    // set_gameplay_cue_notify_actor_properties). LoopingEffects/ApplicationEffects are
    // protected UObject-ref structs -> deferred (see note). Set-to-fixed-value -> idempotent.
    int32 PropertiesSet = 0;
    if (TSharedPtr<FJsonObject> TagError = GASAuthoring_ApplyCueTagParam(Params, CueCDO, PropertiesSet))
    {
        return TagError;
    }
    PropertiesSet += GASAuthoring_ApplyCueActorProperties(Params, CueCDO);

    if (PropertiesSet == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No properties provided (set at least one of gameplay_cue_tag|auto_destroy_on_remove|auto_destroy_delay|auto_attach_to_owner|is_override|unique_instance_per_instigator|unique_instance_per_source_object|allow_multiple_on_active_events|allow_multiple_while_active_events|num_preallocated_instances)"));
    }

    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Blueprint, Edit))
    {
        return SaveFailed;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("cue_notify_path"), BlueprintPath);
    Result->SetNumberField(TEXT("properties_set"), PropertiesSet);
    Result->SetStringField(TEXT("gameplay_cue_tag"), CueCDO->GameplayCueTag.ToString());
    Result->SetStringField(TEXT("gameplay_cue_name"), CueCDO->GameplayCueName.ToString());
    GASAuthoring_AddCueActorPropertyFields(Result, CueCDO);
    Result->SetStringField(TEXT("note"), TEXT("looping/application effects (particles/sounds) require asset-ref authoring; deferred"));
    Result->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeOkReply(Result);
}

// GameplayEffect component authoring

// MCP:add_gameplay_effect_component - Add one empty UGameplayEffectComponent of a standard type to the GEComponents of a GameplayEffect Blueprint CDO and save without recompiling. Idempotent: an existing component of that type returns already_existed=true. Configure it with the set_gameplay_effect_component_* tools; get_gameplay_effect_components lists them. A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param gameplay_effect_path - Asset path of the GameplayEffect Blueprint, e.g. "/Game/GAS/Effects/GE_Damage".
//   @param component_type - Bare type name, case-insensitive: AssetTags, TargetTags, AdditionalEffects, ChanceToApply, Immunity, TargetTagRequirements, RemoveOther, CustomCanApply, BlockAbilityTags, Abilities or CancelAbilityTags (not the full class name).
//   @returns { gameplay_effect_path: string, component_type: string, component_class: string, component_count: int, component_added: bool, already_existed: bool }
TSharedPtr<FJsonObject> HandleAddGameplayEffectComponent(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("gameplay_effect_path"), BlueprintPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString ComponentType;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("component_type"), ComponentType, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    FString Error;
    UGameplayEffect* GECDO = LoadGameplayEffectCDO(BlueprintPath, Blueprint, Error);
    if (!GECDO)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // Idempotent: a GameplayEffect must not hold two components of the same type
    // (the engine errors "two or more types of X in GE" and drops one). The concrete
    // class name is always "<Type>GameplayEffectComponent", so if one already exists
    // return it instead of adding a duplicate.
    {
        const FString ExpectedClassName = ComponentType + TEXT("GameplayEffectComponent");
        TArray<UObject*> ExistingComps;
        ReadGEComponents(GECDO, ExistingComps);
        for (UObject* Comp : ExistingComps)
        {
            if (Comp && Comp->GetClass()->GetName().Equals(ExpectedClassName, ESearchCase::IgnoreCase))
            {
                TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
                Resp->SetStringField(TEXT("gameplay_effect_path"), BlueprintPath);
                Resp->SetStringField(TEXT("component_type"), ComponentType);
                Resp->SetStringField(TEXT("component_class"), Comp->GetClass()->GetName());
                Resp->SetNumberField(TEXT("component_count"), ExistingComps.Num());
                Resp->SetBoolField(TEXT("component_added"), false);
                Resp->SetBoolField(TEXT("already_existed"), true);
                return mcp_common::MakeOkReply(Resp);
            }
        }
    }

    if (TSharedPtr<FJsonObject> Refused = GASAuthoring_RefuseUnsavableEdit(Blueprint))
    {
        return Refused;
    }
    FGASAuthoringCDOEdit Edit(GECDO);
    GECDO->Modify();

    // Add the component to the GE CDO's GEComponents array via the public AddComponent<T>()
    // template (the protected GEComponents array's only public mutator).
    UClass* ComponentClass = nullptr;
    FString MapError;
    UGameplayEffectComponent* Added = AddGEComponentByType(GECDO, ComponentType, ComponentClass, MapError);
    if (!Added)
    {
        return mcp_common::MakeErrorReply(MapError);
    }

    // Persist the CDO edit. GEComponents is a native UPROPERTY (Instanced) on the
    // UGameplayEffect parent, so the value serializes with the package; mark dirty + save
    // without recompiling (mirrors set_gameplay_effect_duration's rationale).
    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Blueprint, Edit))
    {
        return SaveFailed;
    }

    // Verify by reading the GEComponents array back via reflection.
    TArray<UObject*> Components;
    ReadGEComponents(GECDO, Components);
    const int32 ComponentCount = Components.Num();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("gameplay_effect_path"), BlueprintPath);
    Result->SetStringField(TEXT("component_type"), ComponentType);
    Result->SetStringField(TEXT("component_class"), ComponentClass ? ComponentClass->GetName() : FString());
    Result->SetNumberField(TEXT("component_count"), ComponentCount);
    Result->SetBoolField(TEXT("component_added"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:get_gameplay_effect_components - Read the GEComponents array from a GameplayEffect CDO, round-tripping each component's configured values (granted/asset/blocked tags, chance-to-apply, tag requirements, granted-ability configs)
//   @param gameplay_effect_path - Full asset path of the GE Blueprint
//   @returns { gameplay_effect_path: string, components: [object], component_count: int }
TSharedPtr<FJsonObject> HandleGetGameplayEffectComponents(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("gameplay_effect_path"), BlueprintPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = nullptr;
    FString Error;
    UGameplayEffect* GECDO = LoadGameplayEffectCDO(BlueprintPath, Blueprint, Error);
    if (!GECDO)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    TArray<UObject*> Components;
    ReadGEComponents(GECDO, Components);

    // Round-trip the configured values per component.
    // Each known component exposes its config via a public const getter (tags / chance)
    // or, for the granted abilities (protected GrantAbilityConfigs with no public getter),
    // by reflection over the resolved-by-name struct.
    UScriptStruct* SpecConfigStruct = FindFirstObject<UScriptStruct>(
        TEXT("GameplayAbilitySpecConfig"), EFindFirstObjectOptions::NativeFirst);

    TArray<TSharedPtr<FJsonValue>> ComponentsJson;
    for (UObject* Component : Components)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("class"), Component && Component->GetClass() ? Component->GetClass()->GetName() : FString());

        if (UTargetTagsGameplayEffectComponent* TargetTags = Cast<UTargetTagsGameplayEffectComponent>(Component))
        {
            const FInheritedTagContainer& Inh = TargetTags->GetConfiguredTargetTagChanges();
            Entry->SetArrayField(TEXT("target_tags"), mcp_common::GameplayTagsToJson(Inh.Added));
            Entry->SetArrayField(TEXT("combined_tags"), mcp_common::GameplayTagsToJson(Inh.CombinedTags));
        }
        else if (UAssetTagsGameplayEffectComponent* AssetTags = Cast<UAssetTagsGameplayEffectComponent>(Component))
        {
            const FInheritedTagContainer& Inh = AssetTags->GetConfiguredAssetTagChanges();
            Entry->SetArrayField(TEXT("asset_tags"), mcp_common::GameplayTagsToJson(Inh.Added));
            Entry->SetArrayField(TEXT("combined_tags"), mcp_common::GameplayTagsToJson(Inh.CombinedTags));
        }
        else if (UBlockAbilityTagsGameplayEffectComponent* BlockTags = Cast<UBlockAbilityTagsGameplayEffectComponent>(Component))
        {
            const FInheritedTagContainer& Inh = BlockTags->GetConfiguredBlockedAbilityTagChanges();
            Entry->SetArrayField(TEXT("block_ability_tags"), mcp_common::GameplayTagsToJson(Inh.Added));
            Entry->SetArrayField(TEXT("combined_tags"), mcp_common::GameplayTagsToJson(Inh.CombinedTags));
        }
        else if (UChanceToApplyGameplayEffectComponent* Chance = Cast<UChanceToApplyGameplayEffectComponent>(Component))
        {
            Entry->SetNumberField(TEXT("chance_to_apply"), Chance->GetChanceToApplyToTarget().GetValueAtLevel(0.f));
        }
        else if (UTargetTagRequirementsGameplayEffectComponent* Reqs = Cast<UTargetTagRequirementsGameplayEffectComponent>(Component))
        {
            Entry->SetArrayField(TEXT("application_require_tags"), mcp_common::GameplayTagsToJson(Reqs->ApplicationTagRequirements.RequireTags));
            Entry->SetArrayField(TEXT("application_ignore_tags"), mcp_common::GameplayTagsToJson(Reqs->ApplicationTagRequirements.IgnoreTags));
            Entry->SetArrayField(TEXT("ongoing_require_tags"), mcp_common::GameplayTagsToJson(Reqs->OngoingTagRequirements.RequireTags));
            Entry->SetArrayField(TEXT("ongoing_ignore_tags"), mcp_common::GameplayTagsToJson(Reqs->OngoingTagRequirements.IgnoreTags));
            Entry->SetArrayField(TEXT("removal_require_tags"), mcp_common::GameplayTagsToJson(Reqs->RemovalTagRequirements.RequireTags));
            Entry->SetArrayField(TEXT("removal_ignore_tags"), mcp_common::GameplayTagsToJson(Reqs->RemovalTagRequirements.IgnoreTags));
        }
        else if (Component && Component->IsA<UAbilitiesGameplayEffectComponent>())
        {
            // GrantAbilityConfigs is a protected TArray<FGameplayAbilitySpecConfig> with no public
            // getter — read it via reflection (the array property reflects regardless of C++ access).
            TArray<TSharedPtr<FJsonValue>> AbilityConfigsJson;
            if (FArrayProperty* ArrayProp = FindFProperty<FArrayProperty>(
                    UAbilitiesGameplayEffectComponent::StaticClass(), TEXT("GrantAbilityConfigs")))
            {
                if (FStructProperty* InnerStruct = CastField<FStructProperty>(ArrayProp->Inner))
                {
                    UScriptStruct* ElemStruct = SpecConfigStruct ? SpecConfigStruct : InnerStruct->Struct.Get();
                    const void* ArrayValuePtr = ArrayProp->ContainerPtrToValuePtr<void>(Component);
                    FScriptArrayHelper Helper(ArrayProp, ArrayValuePtr);
                    for (int32 Index = 0; Index < Helper.Num(); ++Index)
                    {
                        AbilityConfigsJson.Add(MakeShared<FJsonValueObject>(
                            AbilitySpecConfigToJson(Helper.GetRawPtr(Index), ElemStruct)));
                    }
                }
            }
            Entry->SetArrayField(TEXT("ability_configs"), AbilityConfigsJson);
        }

        ComponentsJson.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("gameplay_effect_path"), BlueprintPath);
    Result->SetArrayField(TEXT("components"), ComponentsJson);
    Result->SetNumberField(TEXT("component_count"), Components.Num());
    return mcp_common::MakeOkReply(Result);
}

// MCP:remove_gameplay_effect_component - Remove a component from a GameplayEffect CDO's GEComponents array by index or by component_type/component_class (inverse of add_gameplay_effect_component), and save; the removed component is left out of the saved asset. A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param gameplay_effect_path - Full asset path of the GE Blueprint
//   @param component_index - Optional index into the GEComponents array (call get_gameplay_effect_components to enumerate; 0 = first). Supply this OR component_type
//   @param component_type - Optional friendly type (AssetTags, TargetTags, AdditionalEffects, ChanceToApply, Immunity, TargetTagRequirements, RemoveOther, CustomCanApply, BlockAbilityTags, Abilities, CancelAbilityTags). Accepts the bare type or the full class name. Supply this OR component_index
//   @param component_class - Alias of component_type (accepts the full "<Type>GameplayEffectComponent" class name)
//   @returns { gameplay_effect_path: string, removed_index: int, removed_class: string, component_count: int, removed: bool, saved: bool }
TSharedPtr<FJsonObject> HandleRemoveGameplayEffectComponent(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("gameplay_effect_path"), BlueprintPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // Selection: exactly one of component_index (by position) or component_type/component_class
    // (by concrete class). component_class is an alias of component_type.
    double ComponentIndexD = 0.0;
    const bool bHasIndex = Params->TryGetNumberField(TEXT("component_index"), ComponentIndexD);
    FString ComponentType;
    const bool bHasType = Params->TryGetStringField(TEXT("component_type"), ComponentType)
        || Params->TryGetStringField(TEXT("component_class"), ComponentType);
    if (!bHasIndex && !bHasType)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing selection: supply component_index (int) or component_type/component_class (string)"));
    }
    if (bHasIndex && bHasType)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Ambiguous selection: supply only one of component_index or component_type/component_class"));
    }

    UBlueprint* Blueprint = nullptr;
    FString Error;
    UGameplayEffect* GECDO = LoadGameplayEffectCDO(BlueprintPath, Blueprint, Error);
    if (!GECDO)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // Snapshot the current components (same reflection reader as get_gameplay_effect_components)
    // so we can report an honest count in every branch, including the no-op ones.
    TArray<UObject*> Before;
    ReadGEComponents(GECDO, Before);

    // GEComponents is a protected TArray<TObjectPtr<UGameplayEffectComponent>> with no public
    // RemoveComponent — resolve the property + a mutable array helper via reflection (the exact
    // dual of ReadGEComponents, just non-const because we mutate).
    FArrayProperty* ArrayProp = FindFProperty<FArrayProperty>(UGameplayEffect::StaticClass(), TEXT("GEComponents"));
    FObjectPropertyBase* InnerObjProp = ArrayProp ? CastField<FObjectPropertyBase>(ArrayProp->Inner) : nullptr;
    if (!ArrayProp || !InnerObjProp)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Reflection failure: could not resolve UGameplayEffect::GEComponents array property"));
    }
    void* ArrayValuePtr = ArrayProp->ContainerPtrToValuePtr<void>(GECDO);
    FScriptArrayHelper Helper(ArrayProp, ArrayValuePtr);

    // Determine the element to remove (index + class captured BEFORE the mutation).
    int32 RemovedIndex = INDEX_NONE;
    FString RemovedClass;

    if (bHasIndex)
    {
        const int32 ComponentIndex = (int32)ComponentIndexD;
        // Out-of-range index -> honest error with the real count (mirror of remove_gameplay_effect_modifier).
        if (!Helper.IsValidIndex(ComponentIndex))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("component_index %d out of range (GEComponents has %d entries; call get_gameplay_effect_components to enumerate)"),
                    ComponentIndex, Helper.Num()));
        }
        RemovedIndex = ComponentIndex;
        if (UObject* Obj = InnerObjProp->GetObjectPropertyValue(Helper.GetRawPtr(ComponentIndex)))
        {
            RemovedClass = Obj->GetClass()->GetName();
        }
    }
    else
    {
        // By class: the concrete class name is always "<Type>GameplayEffectComponent" (the same
        // invariant the add-path idempotency guard relies on), so accept either the bare type or
        // the full class name. Iterate IN REVERSE and remove the first (last-positioned) match.
        const FString ExpectedClassName = ComponentType.EndsWith(TEXT("GameplayEffectComponent"))
            ? ComponentType
            : ComponentType + TEXT("GameplayEffectComponent");
        for (int32 i = Helper.Num() - 1; i >= 0; --i)
        {
            UObject* Obj = InnerObjProp->GetObjectPropertyValue(Helper.GetRawPtr(i));
            if (Obj && Obj->GetClass()->GetName().Equals(ExpectedClassName, ESearchCase::IgnoreCase))
            {
                RemovedIndex = i;
                RemovedClass = Obj->GetClass()->GetName();
                break;
            }
        }
        // No match -> honest no-op (not an error): the effect simply has no such component.
        if (RemovedIndex == INDEX_NONE)
        {
            TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
            Resp->SetStringField(TEXT("gameplay_effect_path"), BlueprintPath);
            Resp->SetNumberField(TEXT("removed_index"), -1);
            Resp->SetStringField(TEXT("removed_class"), FString());
            Resp->SetNumberField(TEXT("component_count"), Before.Num());
            Resp->SetBoolField(TEXT("removed"), false);
            Resp->SetBoolField(TEXT("saved"), false);
            return mcp_common::MakeOkReply(Resp);
        }
    }

    if (TSharedPtr<FJsonObject> Refused = GASAuthoring_RefuseUnsavableEdit(Blueprint))
    {
        return Refused;
    }

    // Mutate the CDO array, then persist without recompiling (GEComponents is a native Instanced
    // UPROPERTY on the UGameplayEffect parent CDO; it serializes with the package — same rationale
    // as the add path). RemoveValues shifts trailing entries down, re-indexing them contiguously.
    FGASAuthoringCDOEdit Edit(GECDO);
    GECDO->Modify();
    UObject* RemovedComponent = InnerObjProp->GetObjectPropertyValue(Helper.GetRawPtr(RemovedIndex));
    Helper.RemoveValues(RemovedIndex, 1);

    // SaveAssetPackage takes every RF_Public object of the package as a root of the save, referenced or not, so
    // the removed component (public like every subobject of the CDO) would still be written, as an orphan
    // export. Without the flag it is saved only while something references it again, as after an undo.
    const EObjectFlags RemovedRootFlags = RemovedComponent ? RemovedComponent->GetMaskedFlags(RF_Public | RF_Standalone) : RF_NoFlags;
    if (RemovedComponent)
    {
        RemovedComponent->ClearFlags(RemovedRootFlags);
    }

    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Blueprint, Edit))
    {
        if (RemovedComponent)
        {
            RemovedComponent->SetFlags(RemovedRootFlags);
        }
        return SaveFailed;
    }

    // Read back the count from the same CDO (no recompile occurred).
    TArray<UObject*> After;
    ReadGEComponents(GECDO, After);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("gameplay_effect_path"), BlueprintPath);
    Result->SetNumberField(TEXT("removed_index"), RemovedIndex);
    Result->SetStringField(TEXT("removed_class"), RemovedClass);
    Result->SetNumberField(TEXT("component_count"), After.Num());
    Result->SetBoolField(TEXT("removed"), true);
    return mcp_common::MakeSavedReply(Result, /*bSaved*/ true, TEXT("remove_gameplay_effect_component"));
}

// ============================================================================
// GameplayEffectComponent CONFIG SETTERS
// add_gameplay_effect_component adds an EMPTY component; these configure it so
// it actually does something.
// All use FindOrAddComponent<T>() (header-inline, cross-module safe) so the setter
// is self-sufficient (no prior add_gameplay_effect_component required) AND a re-run
// mutates the existing component instead of stacking a duplicate.
// ============================================================================

namespace
{
    // A GameplayEffectComponent tag setter's request: the GE Blueprint, and its tags parsed into a
    // registered container plus the resolved/unknown names.
    struct FGASAuthoringTagRequest
    {
        UBlueprint* Blueprint = nullptr;
        FString BlueprintPath;
        FGameplayTagContainer Tags;
        TArray<TSharedPtr<FJsonValue>> Resolved;
        TArray<TSharedPtr<FJsonValue>> Unknown;
    };

    // Shared front of every GameplayEffectComponent tag setter: parse ge_blueprint_path, load the GE CDO,
    // and parse the (required) tags array into Out. Returns the GE CDO, or nullptr + OutErrorResponse on
    // any failure. TagsParamName/required mirror the callers.
    UGameplayEffect* LoadGEAndParseTags(const TSharedPtr<FJsonObject>& Params, const TCHAR* TagsParamName,
                                        FGASAuthoringTagRequest& Out, TSharedPtr<FJsonObject>& OutErrorResponse)
    {
        OutErrorResponse = mcp_common::ReadRequiredString(Params, TEXT("ge_blueprint_path"), Out.BlueprintPath, mcp_common::EAllowEmpty::Yes);
        if (OutErrorResponse.IsValid())
        {
            return nullptr;
        }
        const TArray<TSharedPtr<FJsonValue>>* TagsArray = nullptr;
        if (!Params->TryGetArrayField(TagsParamName, TagsArray) || !TagsArray)
        {
            OutErrorResponse = mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Missing required parameter: %s (array of gameplay tag strings)"), TagsParamName));
            return nullptr;
        }

        FString Error;
        UGameplayEffect* GECDO = LoadGameplayEffectCDO(Out.BlueprintPath, Out.Blueprint, Error);
        if (!GECDO)
        {
            OutErrorResponse = mcp_common::MakeErrorReply(Error);
            return nullptr;
        }
        // Every caller writes the CDO next, so a save that would be refused stops it here.
        OutErrorResponse = GASAuthoring_RefuseUnsavableEdit(Out.Blueprint);
        if (OutErrorResponse.IsValid())
        {
            return nullptr;
        }

        GASAuthoring_ParseTags(*TagsArray, Out.Tags, Out.Resolved, Out.Unknown);
        return GECDO;
    }

    // A tag setter's reply: the tags it resolved, and the component's Added tags read back under TagsField.
    TSharedPtr<FJsonObject> GASAuthoring_TagComponentResult(const FGASAuthoringTagRequest& Request, const UGameplayEffectComponent& Component,
                                                            const TCHAR* TagsField, const FInheritedTagContainer& Readback, bool bAlreadyExisted)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("ge_blueprint_path"), Request.BlueprintPath);
        Result->SetStringField(TEXT("component_class"), Component.GetClass()->GetName());
        Result->SetArrayField(TEXT("resolved_tags"), Request.Resolved);
        Result->SetArrayField(TEXT("unknown_tags"), Request.Unknown);
        Result->SetArrayField(TagsField, mcp_common::GameplayTagsToJson(Readback.Added));
        Result->SetNumberField(TEXT("tag_count"), Readback.Added.Num());
        Result->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
        Result->SetBoolField(TEXT("set"), Readback.Added.Num() == Request.Resolved.Num());
        return Result;
    }

    // FindOrAddComponent<TComponent> on the GE CDO, after Modify(). bOutAlreadyExisted says whether the GE
    // already had that component (the GEComponents count does not grow).
    template <typename TComponent>
    TComponent& GASAuthoring_FindOrAddGEComponent(UGameplayEffect* GECDO, bool& bOutAlreadyExisted)
    {
        TArray<UObject*> Before;
        ReadGEComponents(GECDO, Before);
        GECDO->Modify();
        TComponent& Component = GECDO->FindOrAddComponent<TComponent>();
        TArray<UObject*> After;
        ReadGEComponents(GECDO, After);
        bOutAlreadyExisted = (After.Num() == Before.Num());
        return Component;
    }
}

// The tag setters call the exported SetAndApply* method: it runs UpdateInheritedTagProperties + ApplyTo, the
// part that actually wires the grant, and the FInheritedTagContainer UPROPERTY is private, so reflection
// alone would silently no-op. Engine entry point per setter:
//   set_..._target_tags        -> UTargetTagsGameplayEffectComponent::SetAndApplyTargetTagChanges
//   set_..._asset_tags         -> UAssetTagsGameplayEffectComponent::SetAndApplyAssetTagChanges
//   set_..._block_ability_tags -> UBlockAbilityTagsGameplayEffectComponent::SetAndApplyBlockedAbilityTagChanges
//   set_..._chance_to_apply    -> UChanceToApplyGameplayEffectComponent::SetChanceToApplyToTarget(FScalableFloat)
//   set_..._target_tag_requirements -> UTargetTagRequirementsGameplayEffectComponent public
//       Application/Ongoing/RemovalTagRequirements (FGameplayTagRequirements RequireTags/IgnoreTags)
//   set_..._granted_abilities  -> UAbilitiesGameplayEffectComponent::AddGrantedAbilityConfig (AddUnique dedup)

// MCP:set_gameplay_effect_component_target_tags - Configure a TargetTagsGameplayEffectComponent so the GE GRANTS the given tags to its target actor (creates the component if absent; idempotent via FindOrAddComponent). A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param ge_blueprint_path - Full asset path of the GE Blueprint
//   @param tags - List of gameplay tag strings to grant to the target (replaces the component's Added tags)
//   @returns { ge_blueprint_path: string, component_class: string, resolved_tags: [string], unknown_tags: [string], granted_tags: [string], tag_count: int, already_existed: bool, set: bool }
TSharedPtr<FJsonObject> HandleSetGameplayEffectComponentTargetTags(const TSharedPtr<FJsonObject>& Params)
{
    FGASAuthoringTagRequest Request;
    TSharedPtr<FJsonObject> ErrResp;
    UGameplayEffect* GECDO = LoadGEAndParseTags(Params, TEXT("tags"), Request, ErrResp);
    if (!GECDO)
    {
        return ErrResp;
    }

    // Idempotent: FindOrAddComponent reuses an existing component (so a re-run mutates it).
    FGASAuthoringCDOEdit Edit(GECDO);
    bool bAlreadyExisted = false;
    UTargetTagsGameplayEffectComponent& Comp = GASAuthoring_FindOrAddGEComponent<UTargetTagsGameplayEffectComponent>(GECDO, bAlreadyExisted);

    // The InheritableGrantedTagsContainer UPROPERTY is PRIVATE — reflection-write would no-op.
    // The exported SetAndApplyTargetTagChanges runs UpdateInheritedTagProperties + ApplyTo
    // (the part that actually wires the grant onto the GE's CachedGrantedTags).
    FInheritedTagContainer Inherited;
    Inherited.Added = Request.Tags;
    Comp.SetAndApplyTargetTagChanges(Inherited);

    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Request.Blueprint, Edit))
    {
        return SaveFailed;
    }

    TSharedPtr<FJsonObject> Result = GASAuthoring_TagComponentResult(Request, Comp, TEXT("granted_tags"), Comp.GetConfiguredTargetTagChanges(), bAlreadyExisted);
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_gameplay_effect_component_asset_tags - Configure an AssetTagsGameplayEffectComponent so the GE OWNS the given asset tags (does not transfer to actors; creates the component if absent; idempotent via FindOrAddComponent). A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param ge_blueprint_path - Full asset path of the GE Blueprint
//   @param tags - List of gameplay tag strings the GE asset owns (replaces the component's Added tags)
//   @returns { ge_blueprint_path: string, component_class: string, resolved_tags: [string], unknown_tags: [string], asset_tags: [string], tag_count: int, already_existed: bool, set: bool }
TSharedPtr<FJsonObject> HandleSetGameplayEffectComponentAssetTags(const TSharedPtr<FJsonObject>& Params)
{
    FGASAuthoringTagRequest Request;
    TSharedPtr<FJsonObject> ErrResp;
    UGameplayEffect* GECDO = LoadGEAndParseTags(Params, TEXT("tags"), Request, ErrResp);
    if (!GECDO)
    {
        return ErrResp;
    }

    FGASAuthoringCDOEdit Edit(GECDO);
    bool bAlreadyExisted = false;
    UAssetTagsGameplayEffectComponent& Comp = GASAuthoring_FindOrAddGEComponent<UAssetTagsGameplayEffectComponent>(GECDO, bAlreadyExisted);

    FInheritedTagContainer Inherited;
    Inherited.Added = Request.Tags;
    Comp.SetAndApplyAssetTagChanges(Inherited);

    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Request.Blueprint, Edit))
    {
        return SaveFailed;
    }

    TSharedPtr<FJsonObject> Result = GASAuthoring_TagComponentResult(Request, Comp, TEXT("asset_tags"), Comp.GetConfiguredAssetTagChanges(), bAlreadyExisted);
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_gameplay_effect_component_block_ability_tags - Configure a BlockAbilityTagsGameplayEffectComponent so the GE BLOCKS abilities with the given tags on its target (creates the component if absent; idempotent via FindOrAddComponent). A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param ge_blueprint_path - Full asset path of the GE Blueprint
//   @param tags - List of gameplay tag strings of abilities to block (replaces the component's Added tags)
//   @returns { ge_blueprint_path: string, component_class: string, resolved_tags: [string], unknown_tags: [string], block_ability_tags: [string], tag_count: int, already_existed: bool, set: bool }
TSharedPtr<FJsonObject> HandleSetGameplayEffectComponentBlockAbilityTags(const TSharedPtr<FJsonObject>& Params)
{
    FGASAuthoringTagRequest Request;
    TSharedPtr<FJsonObject> ErrResp;
    UGameplayEffect* GECDO = LoadGEAndParseTags(Params, TEXT("tags"), Request, ErrResp);
    if (!GECDO)
    {
        return ErrResp;
    }

    FGASAuthoringCDOEdit Edit(GECDO);
    bool bAlreadyExisted = false;
    UBlockAbilityTagsGameplayEffectComponent& Comp = GASAuthoring_FindOrAddGEComponent<UBlockAbilityTagsGameplayEffectComponent>(GECDO, bAlreadyExisted);

    FInheritedTagContainer Inherited;
    Inherited.Added = Request.Tags;
    Comp.SetAndApplyBlockedAbilityTagChanges(Inherited);

    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Request.Blueprint, Edit))
    {
        return SaveFailed;
    }

    TSharedPtr<FJsonObject> Result = GASAuthoring_TagComponentResult(Request, Comp, TEXT("block_ability_tags"), Comp.GetConfiguredBlockedAbilityTagChanges(), bAlreadyExisted);
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_gameplay_effect_component_chance_to_apply - Configure a ChanceToApplyGameplayEffectComponent with the probability (0.0..1.0) that the GE applies to its target (creates the component if absent; idempotent via FindOrAddComponent + set-to-fixed-value). A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param ge_blueprint_path - Full asset path of the GE Blueprint
//   @param chance - Probability the GE applies (0.0 = never, 1.0 = always)
//   @returns { ge_blueprint_path: string, component_class: string, chance: float, chance_readback: float, chance_matches: bool, already_existed: bool, set: bool }
TSharedPtr<FJsonObject> HandleSetGameplayEffectComponentChanceToApply(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ge_blueprint_path"), BlueprintPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    double Chance = 0.0;
    if (!Params->TryGetNumberField(TEXT("chance"), Chance))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: chance (0.0..1.0)"));
    }

    UBlueprint* Blueprint = nullptr;
    FString Error;
    UGameplayEffect* GECDO = LoadGameplayEffectCDO(BlueprintPath, Blueprint, Error);
    if (!GECDO)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    if (TSharedPtr<FJsonObject> Refused = GASAuthoring_RefuseUnsavableEdit(Blueprint))
    {
        return Refused;
    }

    FGASAuthoringCDOEdit Edit(GECDO);
    bool bAlreadyExisted = false;
    UChanceToApplyGameplayEffectComponent& Comp = GASAuthoring_FindOrAddGEComponent<UChanceToApplyGameplayEffectComponent>(GECDO, bAlreadyExisted);

    // SetChanceToApplyToTarget is the exported mutator of the private ChanceToApplyToTarget
    // FScalableFloat. Build a constant FScalableFloat from the float param (no curve).
    Comp.SetChanceToApplyToTarget(FScalableFloat((float)Chance));

    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Blueprint, Edit))
    {
        return SaveFailed;
    }

    const float Readback = Comp.GetChanceToApplyToTarget().GetValueAtLevel(0.f);
    const bool bMatches = FMath::IsNearlyEqual(Readback, (float)Chance, 0.001f);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("ge_blueprint_path"), BlueprintPath);
    Result->SetStringField(TEXT("component_class"), Comp.GetClass()->GetName());
    Result->SetNumberField(TEXT("chance"), Chance);
    Result->SetNumberField(TEXT("chance_readback"), Readback);
    Result->SetBoolField(TEXT("chance_matches"), bMatches);
    Result->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Result->SetBoolField(TEXT("set"), bMatches);
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_gameplay_effect_component_target_tag_requirements - Configure a TargetTagRequirementsGameplayEffectComponent's Application/Ongoing/Removal require & ignore tags (creates the component if absent; idempotent via FindOrAddComponent + set-to-fixed-value). A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param ge_blueprint_path - Full asset path of the GE Blueprint
//   @param application_require_tags [array] - Optional array of gameplay tag strings: the target must have for the GE to apply
//   @param application_ignore_tags [array] - Optional array of gameplay tag strings: that, if present, block application
//   @param ongoing_require_tags [array] - Optional array of gameplay tag strings: the target must keep for the GE to stay 'on'
//   @param ongoing_ignore_tags [array] - Optional array of gameplay tag strings: that, if present, turn the GE 'off' while applied
//   @param removal_require_tags [array] - Optional array of gameplay tag strings: that, if met, remove the GE
//   @param removal_ignore_tags [array] - Optional array of gameplay tag strings: ignore tags for the removal requirement
//   @returns { ge_blueprint_path: string, component_class: string, already_existed: bool, application_require_tags: [string], application_ignore_tags: [string], ongoing_require_tags: [string], ongoing_ignore_tags: [string], removal_require_tags: [string], removal_ignore_tags: [string], unknown_tags: [string], set: bool }
TSharedPtr<FJsonObject> HandleSetGameplayEffectComponentTargetTagRequirements(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ge_blueprint_path"), BlueprintPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // At least one of the 6 tag lists is required (honest: no-op otherwise). Checked before anything is
    // written: FindOrAddComponent below adds the component when the GE has none.
    struct FReqField { const TCHAR* Param; FGameplayTagContainer* Target; const TArray<TSharedPtr<FJsonValue>>* Array; };
    static const TCHAR* const ListParams[] = {
        TEXT("application_require_tags"), TEXT("application_ignore_tags"),
        TEXT("ongoing_require_tags"),     TEXT("ongoing_ignore_tags"),
        TEXT("removal_require_tags"),     TEXT("removal_ignore_tags"),
    };
    bool bAnyProvided = false;
    for (const TCHAR* ListParam : ListParams)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        bAnyProvided = bAnyProvided || (Params->TryGetArrayField(ListParam, Arr) && Arr);
    }
    if (!bAnyProvided)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Provide at least one tag list (application_/ongoing_/removal_ require_tags / ignore_tags)"));
    }

    UBlueprint* Blueprint = nullptr;
    FString Error;
    UGameplayEffect* GECDO = LoadGameplayEffectCDO(BlueprintPath, Blueprint, Error);
    if (!GECDO)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    if (TSharedPtr<FJsonObject> Refused = GASAuthoring_RefuseUnsavableEdit(Blueprint))
    {
        return Refused;
    }

    FGASAuthoringCDOEdit Edit(GECDO);
    bool bAlreadyExisted = false;
    UTargetTagRequirementsGameplayEffectComponent& Comp = GASAuthoring_FindOrAddGEComponent<UTargetTagRequirementsGameplayEffectComponent>(GECDO, bAlreadyExisted);

    // The three FGameplayTagRequirements UPROPERTYs are PUBLIC with public RequireTags/IgnoreTags
    // FGameplayTagContainers — set them directly (no exported method needed). Each provided list
    // fully REPLACES its field (set-to-fixed-value, idempotent); omitted fields are untouched.
    FReqField Fields[] = {
        { TEXT("application_require_tags"), &Comp.ApplicationTagRequirements.RequireTags, nullptr },
        { TEXT("application_ignore_tags"),  &Comp.ApplicationTagRequirements.IgnoreTags,  nullptr },
        { TEXT("ongoing_require_tags"),     &Comp.OngoingTagRequirements.RequireTags,     nullptr },
        { TEXT("ongoing_ignore_tags"),      &Comp.OngoingTagRequirements.IgnoreTags,      nullptr },
        { TEXT("removal_require_tags"),     &Comp.RemovalTagRequirements.RequireTags,     nullptr },
        { TEXT("removal_ignore_tags"),      &Comp.RemovalTagRequirements.IgnoreTags,      nullptr },
    };

    TArray<TSharedPtr<FJsonValue>> AllUnknown;
    for (FReqField& F : Fields)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Params->TryGetArrayField(F.Param, Arr) && Arr)
        {
            FGameplayTagContainer Container;
            TArray<TSharedPtr<FJsonValue>> Resolved, Unknown;
            GASAuthoring_ParseTags(*Arr, Container, Resolved, Unknown);
            *F.Target = Container;
            AllUnknown.Append(Unknown);
        }
    }

    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Blueprint, Edit))
    {
        return SaveFailed;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("ge_blueprint_path"), BlueprintPath);
    Result->SetStringField(TEXT("component_class"), Comp.GetClass()->GetName());
    Result->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Result->SetArrayField(TEXT("application_require_tags"), mcp_common::GameplayTagsToJson(Comp.ApplicationTagRequirements.RequireTags));
    Result->SetArrayField(TEXT("application_ignore_tags"), mcp_common::GameplayTagsToJson(Comp.ApplicationTagRequirements.IgnoreTags));
    Result->SetArrayField(TEXT("ongoing_require_tags"), mcp_common::GameplayTagsToJson(Comp.OngoingTagRequirements.RequireTags));
    Result->SetArrayField(TEXT("ongoing_ignore_tags"), mcp_common::GameplayTagsToJson(Comp.OngoingTagRequirements.IgnoreTags));
    Result->SetArrayField(TEXT("removal_require_tags"), mcp_common::GameplayTagsToJson(Comp.RemovalTagRequirements.RequireTags));
    Result->SetArrayField(TEXT("removal_ignore_tags"), mcp_common::GameplayTagsToJson(Comp.RemovalTagRequirements.IgnoreTags));
    Result->SetArrayField(TEXT("unknown_tags"), AllUnknown);
    Result->SetBoolField(TEXT("set"), AllUnknown.Num() == 0);
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_gameplay_effect_component_granted_abilities - Configure an AbilitiesGameplayEffectComponent to grant a GameplayAbility (by BP path/class name) to the target while the GE is active (creates the component if absent; AddGrantedAbilityConfig dedups via AddUnique). A running PIE session, a read-only root or a .uasset read-only on disk fails before any edit, and a failed save undoes the edit.
//   @param ge_blueprint_path - Full asset path of the GE Blueprint
//   @param ability_path - GameplayAbility Blueprint asset path or bare class name to grant
//   @param input_id - Optional input ID to bind the granted ability to (default -1 = none)
//   @returns { ge_blueprint_path: string, component_class: string, ability_class: string, ability_config_count: int, input_id: int, already_existed: bool, ability_resolved: bool, set: bool }
TSharedPtr<FJsonObject> HandleSetGameplayEffectComponentGrantedAbilities(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ge_blueprint_path"), BlueprintPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString AbilityPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ability_path"), AbilityPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 InputID = INDEX_NONE;
    double InputIDD = 0.0;
    if (Params->TryGetNumberField(TEXT("input_id"), InputIDD))
    {
        InputID = (int32)InputIDD;
    }

    // Resolve the ability class by BP path or bare name (ResolveClass handles both).
    UClass* AbilityClass = FUEAgentGraphUtils::ResolveClass(AbilityPath);
    if (!AbilityClass || !AbilityClass->IsChildOf(UGameplayAbility::StaticClass()))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("ability_path did not resolve to a UGameplayAbility subclass: %s"), *AbilityPath));
    }

    UBlueprint* Blueprint = nullptr;
    FString Error;
    UGameplayEffect* GECDO = LoadGameplayEffectCDO(BlueprintPath, Blueprint, Error);
    if (!GECDO)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    if (TSharedPtr<FJsonObject> Refused = GASAuthoring_RefuseUnsavableEdit(Blueprint))
    {
        return Refused;
    }

    FGASAuthoringCDOEdit Edit(GECDO);
    bool bAlreadyExisted = false;
    UAbilitiesGameplayEffectComponent& Comp = GASAuthoring_FindOrAddGEComponent<UAbilitiesGameplayEffectComponent>(GECDO, bAlreadyExisted);

    // AddGrantedAbilityConfig appends with AddUnique (dedup), so re-granting the same ability
    // at the same config is idempotent (no duplicate entry).
    FGameplayAbilitySpecConfig Config;
    Config.Ability = AbilityClass;
    Config.InputID = InputID;
    Comp.AddGrantedAbilityConfig(Config);

    if (TSharedPtr<FJsonObject> SaveFailed = GASAuthoring_SaveBlueprint(Blueprint, Edit))
    {
        return SaveFailed;
    }

    // Read back the granted-config count via reflection (GrantAbilityConfigs is protected, no getter).
    int32 ConfigCount = 0;
    bool bAbilityFound = false;
    if (FArrayProperty* ArrayProp = FindFProperty<FArrayProperty>(
            UAbilitiesGameplayEffectComponent::StaticClass(), TEXT("GrantAbilityConfigs")))
    {
        const void* ArrayValuePtr = ArrayProp->ContainerPtrToValuePtr<void>(&Comp);
        FScriptArrayHelper Helper(ArrayProp, ArrayValuePtr);
        ConfigCount = Helper.Num();
        if (FStructProperty* InnerStruct = CastField<FStructProperty>(ArrayProp->Inner))
        {
            if (FClassProperty* AbilityProp = FindFProperty<FClassProperty>(InnerStruct->Struct, TEXT("Ability")))
            {
                for (int32 Index = 0; Index < Helper.Num(); ++Index)
                {
                    UObject* Val = AbilityProp->GetObjectPropertyValue(
                        AbilityProp->ContainerPtrToValuePtr<void>(Helper.GetRawPtr(Index)));
                    if (Cast<UClass>(Val) == AbilityClass)
                    {
                        bAbilityFound = true;
                        break;
                    }
                }
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("ge_blueprint_path"), BlueprintPath);
    Result->SetStringField(TEXT("component_class"), Comp.GetClass()->GetName());
    Result->SetStringField(TEXT("ability_class"), AbilityClass->GetName());
    Result->SetNumberField(TEXT("ability_config_count"), ConfigCount);
    Result->SetNumberField(TEXT("input_id"), InputID);
    Result->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Result->SetBoolField(TEXT("ability_resolved"), true);
    Result->SetBoolField(TEXT("set"), bAbilityFound);
    return mcp_common::MakeOkReply(Result);
}

// ============================================================================
// GAS extras: runtime ASC queries + ability info
// ============================================================================

// MCP:get_ability_activation_info - Read the AssetTags of a GameplayAbility Blueprint from its class default object: ability_tags, asset_tag_count, and ability_name (the default object name). Despite the tool name, no activation data is read: no activation policy, required or blocked tags, cost or cooldown. Read-only and editor-time: it loads the asset and needs no actor or PIE; a path that is not a GameplayAbility Blueprint is an error. configure_ability_tags writes these tags; get_ability_info reads an ability granted to an actor.
//   @param ability_path - Full /Game path to the ability Blueprint asset
//   @returns { ability_path: string, ability_name: string, ability_tags: [string], asset_tag_count: int }
TSharedPtr<FJsonObject> HandleGetAbilityActivationInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString AbilityPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ability_path"), AbilityPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // Load the ability blueprint and get its CDO
    UBlueprint* Blueprint = nullptr;
    FString Error;
    UGameplayAbility* AbilityCDO = GASAuthoring_LoadBlueprintCDO<UGameplayAbility>(AbilityPath, TEXT("Ability"), Blueprint, Error);
    if (!AbilityCDO)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // Read tags (public UPROPERTY on UGameplayAbility)
    // Note: BlockAbilitiesWithTag and CancelAbilitiesWithTag are protected
    const FGameplayTagContainer& AssetTags = AbilityCDO->GetAssetTags();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("ability_path"), AbilityPath);
    Result->SetStringField(TEXT("ability_name"), AbilityCDO->GetName());
    Result->SetArrayField(TEXT("ability_tags"), mcp_common::GameplayTagsToJson(AssetTags));
    Result->SetNumberField(TEXT("asset_tag_count"), AssetTags.Num());
    return mcp_common::MakeOkReply(Result);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_attribute_to_set"), &HandleAddAttributeToSet },
    { TEXT("add_gameplay_effect_component"), &HandleAddGameplayEffectComponent },
    { TEXT("add_gameplay_effect_modifier"), &HandleAddGameplayEffectModifier },
    { TEXT("configure_ability_tags"), &HandleConfigureAbilityTags },
    { TEXT("create_attribute_set_asset"), &HandleCreateAttributeSetAsset },
    { TEXT("create_attribute_set_blueprint"), &HandleCreateAttributeSetBlueprint },
    { TEXT("create_gameplay_ability_blueprint"), &HandleCreateGameplayAbilityBlueprint },
    { TEXT("create_gameplay_cue_notify_actor"), &HandleCreateGameplayCueNotifyActor },
    { TEXT("create_gameplay_cue_notify_burst"), &HandleCreateGameplayCueNotifyBurst },
    { TEXT("create_gameplay_cue_notify_looping"), &HandleCreateGameplayCueNotifyLooping },
    { TEXT("create_gameplay_cue_notify_static"), &HandleCreateGameplayCueNotifyStatic },
    { TEXT("create_gameplay_effect_blueprint"), &HandleCreateGameplayEffectBlueprint },
    { TEXT("create_gameplay_mod_magnitude_calculation_blueprint"), &HandleCreateGameplayModMagnitudeCalculationBlueprint },
    { TEXT("get_ability_activation_info"), &HandleGetAbilityActivationInfo },
    { TEXT("get_gameplay_cue_notify_info"), &HandleGetGameplayCueNotifyInfo },
    { TEXT("get_gameplay_effect_components"), &HandleGetGameplayEffectComponents },
    { TEXT("remove_gameplay_effect_component"), &HandleRemoveGameplayEffectComponent },
    { TEXT("remove_gameplay_effect_modifier"), &HandleRemoveGameplayEffectModifier },
    { TEXT("set_ability_cooldown"), &HandleSetAbilityCooldown },
    { TEXT("set_ability_cost"), &HandleSetAbilityCost },
    { TEXT("set_gameplay_cue_notify_actor_properties"), &HandleSetGameplayCueNotifyActorProperties },
    { TEXT("set_gameplay_cue_notify_burst_properties"), &HandleSetGameplayCueNotifyBurstProperties },
    { TEXT("set_gameplay_cue_notify_looping_properties"), &HandleSetGameplayCueNotifyLoopingProperties },
    { TEXT("set_gameplay_cue_notify_static_properties"), &HandleSetGameplayCueNotifyStaticProperties },
    { TEXT("set_gameplay_effect_component_asset_tags"), &HandleSetGameplayEffectComponentAssetTags },
    { TEXT("set_gameplay_effect_component_block_ability_tags"), &HandleSetGameplayEffectComponentBlockAbilityTags },
    { TEXT("set_gameplay_effect_component_chance_to_apply"), &HandleSetGameplayEffectComponentChanceToApply },
    { TEXT("set_gameplay_effect_component_granted_abilities"), &HandleSetGameplayEffectComponentGrantedAbilities },
    { TEXT("set_gameplay_effect_component_target_tag_requirements"), &HandleSetGameplayEffectComponentTargetTagRequirements },
    { TEXT("set_gameplay_effect_component_target_tags"), &HandleSetGameplayEffectComponentTargetTags },
    { TEXT("set_gameplay_effect_duration"), &HandleSetGameplayEffectDuration },
    { TEXT("set_gameplay_effect_modifier_attribute_based"), &HandleSetGameplayEffectModifierAttributeBased },
    { TEXT("set_gameplay_effect_modifier_attribute_based_up_to_channel"), &HandleSetGameplayEffectModifierAttributeBasedUpToChannel },
    { TEXT("set_gameplay_effect_modifier_attribute_curve"), &HandleSetGameplayEffectModifierAttributeCurve },
    { TEXT("set_gameplay_effect_modifier_attribute_tag_filters"), &HandleSetGameplayEffectModifierAttributeTagFilters },
    { TEXT("set_gameplay_effect_modifier_custom"), &HandleSetGameplayEffectModifierCustom },
    { TEXT("set_gameplay_effect_modifier_custom_curve"), &HandleSetGameplayEffectModifierCustomCurve },
    { TEXT("set_gameplay_effect_modifier_set_by_caller"), &HandleSetGameplayEffectModifierSetByCaller },
    { TEXT("set_gameplay_effect_period"), &HandleSetGameplayEffectPeriod },
    { TEXT("set_gameplay_effect_stacking"), &HandleSetGameplayEffectStacking },
};
} // namespace UEAgentGASAuthoringCommands
