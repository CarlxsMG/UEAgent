#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

// Engine submix (USoundfieldSubmix + SoundfieldEffectChain + EncodingSettings)
#include "Sound/SoundSubmix.h"

// AudioExtensions plugin (USoundfieldEffectBase + USoundfieldEffectSettingsBase +
// USoundfieldEncodingSettingsBase — all abstract)
#include "ISoundfieldFormat.h"

// Editor / asset plumbing
#include "Editor.h"                          // GEditor
#include "Engine/Engine.h"                   // GEngine
#include "ScopedTransaction.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"         // TObjectIterator (for derived-class scan)
#include "UObject/Class.h"                   // UClass::IsChildOf, HasAnyClassFlags
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"

/**
 * Soundfield submix commands.
 *
 * Backed by USoundfieldSubmix (Engine module, Sound/SoundSubmix.h), a USoundSubmixWithParentBase variant
 * that hosts a SoundfieldEffectChain (TArray<TObjectPtr<USoundfieldEffectBase>>) and an EncodingSettings
 * (TObjectPtr<USoundfieldEncodingSettingsBase>). The effect base class (Settings, GetSettingsClass()) and
 * the abstract encoding settings base come from AudioExtensions (ISoundfieldFormat.h).
 *
 * IMPORTANT, engine stock constraint: USoundfieldEffectBase ships as `abstract` and stock UE 5.8 has NO
 * concrete subclass, so the effect chain of a freshly created soundfield submix is always empty. The
 * add / remove effect commands still implement the FULL path, so they work WITHOUT touching
 * this code the moment a third-party audio plugin (Resonance Audio, Steam Audio, Oculus Audio, ...)
 * provides a concrete subclass; until then they return an honest `note` payload explaining the constraint.
 */
namespace UEAgentSoundfieldCommands
{

// MCP-DOMAIN: Audio

namespace
{
    /**
     * Resolve a friendly-name or `/Script/...` path to a concrete
     * USoundfieldEffectBase subclass (non-abstract, IsChildOf(USoundfieldEffectBase)).
     * Returns nullptr if nothing matches; OutError explains why.
     *
     * Resolution order:
     *   1. UClass::TryFindTypeSlow<UClass>(Name)  — supports paths AND short names
     *   2. LoadObject<UClass>(nullptr, *Name)     — path-based fallback
     * Both candidates are validated: !CLASS_Abstract && IsChildOf(USoundfieldEffectBase).
     */
    UClass* ResolveConcreteSoundfieldEffectClass(const FString& InName, FString& OutError)
    {
        UClass* Found = nullptr;

        // 1) TryFindTypeSlow handles both `/Script/Plugin.Class` and short names
        Found = FindObject<UClass>(nullptr, *InName);
        if (!Found)
        {
            Found = LoadObject<UClass>(nullptr, *InName);
        }

        if (!Found)
        {
            OutError = FString::Printf(
                TEXT("Cannot resolve soundfield effect class '%s' (not a known UClass path)"),
                *InName);
            return nullptr;
        }
        if (!Found->IsChildOf(USoundfieldEffectBase::StaticClass()))
        {
            OutError = FString::Printf(
                TEXT("Class '%s' is not derived from USoundfieldEffectBase"), *InName);
            return nullptr;
        }
        if (Found->HasAnyClassFlags(CLASS_Abstract))
        {
            OutError = FString::Printf(
                TEXT("Class '%s' is abstract; cannot instantiate"), *InName);
            return nullptr;
        }
        return Found;
    }

    /**
     * Detect whether ANY concrete subclass of USoundfieldEffectBase is loadable
     * in the running editor. Used to differentiate "the user gave a bad name"
     * (error) from "the engine simply has no concrete effect classes shipped"
     * (honest gate with `note`).
     *
     * Returns the first concrete subclass found (or nullptr if none).
     */
    UClass* FindAnyConcreteSoundfieldEffectClass()
    {
        TArray<UClass*> Derived;
        GetDerivedClasses(USoundfieldEffectBase::StaticClass(), Derived, /*bRecursive=*/true);
        for (UClass* C : Derived)
        {
            if (C && !C->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
            {
                return C;
            }
        }
        return nullptr;
    }

    /**
     * Serialize one USoundfieldEffectBase (entry in SoundfieldEffectChain) to JSON.
     * Reports class + path + settings_class (when Settings is set). Pure read.
     */
    TSharedPtr<FJsonObject> EffectToJson(int32 Index, const USoundfieldEffectBase* Effect)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("index"), Index);
        if (!Effect)
        {
            Obj->SetStringField(TEXT("class"), TEXT(""));
            Obj->SetStringField(TEXT("path"), TEXT(""));
            Obj->SetStringField(TEXT("settings_class"), TEXT(""));
            return Obj;
        }
        Obj->SetStringField(TEXT("class"), Effect->GetClass()->GetName());
        Obj->SetStringField(TEXT("path"), Effect->GetPathName());
        const USoundfieldEffectSettingsBase* S = Effect->Settings.Get();
        Obj->SetStringField(TEXT("settings_class"), S ? S->GetClass()->GetName() : TEXT(""));
        Obj->SetStringField(TEXT("settings_path"), S ? S->GetPathName() : TEXT(""));
        return Obj;
    }

    /**
     * Returns the standardized honest-gate note for the "no concrete soundfield
     * effect subclasses shipped in stock UE 5.8" case. Same text used by the add,
     * info and remove commands so the wire-level behavior is consistent.
     */
    FString SoundfieldHonestGateNote()
    {
        return TEXT(
            "No concrete USoundfieldEffectBase subclass is loadable in this UE 5.8 install. "
            "USoundfieldEffectBase is shipped as abstract base only (ISoundfieldFormat.h). "
            "Concrete effect classes must be contributed by a third-party audio plugin "
            "(Resonance Audio, Steam Audio, Oculus Audio, MS Spatial Audio with effects, etc.) "
            "-- none of which ship with stock UE 5.8. The submix asset can be created and "
            "inspected (cmds create_soundfield_submix, get_soundfield_submix_info), but "
            "SoundfieldEffectChain manipulation requires a concrete effect class to be available.");
    }

    /** The soundfield submix at SubmixPath. Null on success, else the error reply. */
    TSharedPtr<FJsonObject> Soundfield_LoadSubmix(const FString& SubmixPath, USoundfieldSubmix*& OutSubmix)
    {
        OutSubmix = mcp_common::ResolveAsset<USoundfieldSubmix>(SubmixPath);
        if (!OutSubmix)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("SoundfieldSubmix not found: %s"), *SubmixPath));
        }
        return nullptr;
    }

    /**
     * Reads the required submix_path and effect_index params. Null when both are there and the index is not
     * negative, else the error reply.
     */
    TSharedPtr<FJsonObject> Soundfield_ReadEffectSlot(const TSharedPtr<FJsonObject>& Params, FString& OutSubmixPath,
        int32& OutEffectIndex)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("submix_path"), OutSubmixPath)) { return Missing; }
        double EffectIndexD = -1.0;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("effect_index"), EffectIndexD)) { return Missing; }
        OutEffectIndex = static_cast<int32>(EffectIndexD);
        if (OutEffectIndex < 0)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("effect_index must be >= 0 (got %d)"), OutEffectIndex));
        }
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// MCP:create_soundfield_submix - Create a USoundfieldSubmix asset at submix_path and save it, with its default encoding format and an empty effect chain; a new one succeeds only when it reaches disk. An existing one there is returned unchanged and not re-saved (already_existed true, saved false). Returns soundfield_encoding_format, effect_chain_length and saved; inspect it with get_soundfield_submix_info.
//   @param submix_path - Full asset path like "/Game/Audio/MySFSubmix" (required).
//   @returns { submix_path, asset_path, soundfield_encoding_format, encoding_settings_class, created, already_existed, saved }
// ---------------------------------------------------------------------------
// 1. create_soundfield_submix — CreatePackage + NewObject<USoundfieldSubmix> + SaveAssetPackage.
TSharedPtr<FJsonObject> HandleCreateSoundfieldSubmix(const TSharedPtr<FJsonObject>& Params)
{
    FString SubmixPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("submix_path"), SubmixPath)) { return Missing; }

    // Idempotent: load first
    if (USoundfieldSubmix* Existing = mcp_common::ResolveAsset<USoundfieldSubmix>(SubmixPath))
    {
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("submix_path"), Existing->GetPathName());
        ResultObj->SetStringField(TEXT("soundfield_encoding_format"), Existing->SoundfieldEncodingFormat.ToString());
        ResultObj->SetStringField(TEXT("encoding_settings_class"),
            Existing->EncodingSettings ? Existing->EncodingSettings->GetClass()->GetName() : TEXT(""));
        ResultObj->SetBoolField(TEXT("created"), false);
        ResultObj->SetBoolField(TEXT("already_existed"), true);
        ResultObj->SetBoolField(TEXT("saved"), false);
        ResultObj->SetNumberField(TEXT("effect_chain_length"), Existing->SoundfieldEffectChain.Num());
        return mcp_common::MakeOkReply(ResultObj);
    }

    // Create package + asset
    const FString PackagePath = FPackageName::GetLongPackagePath(SubmixPath);
    const FString AssetName = FPackageName::GetShortName(SubmixPath);
    if (PackagePath.IsEmpty() || AssetName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid submix_path '%s' (must be /Game/Path/Name)"), *SubmixPath));
    }

    FString CreateError;
    USoundfieldSubmix* NewSubmix = mcp_common::NewAssetInPackage<USoundfieldSubmix>(SubmixPath, AssetName,
        TEXT("Failed to NewObject<USoundfieldSubmix>"), CreateError);
    if (!NewSubmix)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }
    FAssetRegistryModule::AssetCreated(NewSubmix);
    NewSubmix->MarkPackageDirty();

    // Save
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(NewSubmix->GetOutermost(), NewSubmix, SaveError);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("submix_path"), NewSubmix->GetPathName());
    ResultObj->SetStringField(TEXT("soundfield_encoding_format"), NewSubmix->SoundfieldEncodingFormat.ToString());
    ResultObj->SetStringField(TEXT("encoding_settings_class"), TEXT(""));
    ResultObj->SetBoolField(TEXT("created"), true);
    ResultObj->SetBoolField(TEXT("already_existed"), false);
    ResultObj->SetNumberField(TEXT("effect_chain_length"), NewSubmix->SoundfieldEffectChain.Num());
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("create_soundfield_submix"), SaveError);
}

// ---------------------------------------------------------------------------
// MCP:add_soundfield_effect_to_submix - Append a new effect of a concrete USoundfieldEffectBase class to a USoundfieldSubmix effect chain and save it; an addition succeeds only when the submix reaches disk. A class already in the chain is skipped (added false, already_present true, not saved). When no concrete subclass is loaded (stock UE 5.8 ships none) nothing changes and the call fails, with added false and a note.
//   @param submix_path - Full path to the USoundfieldSubmix (required).
//   @param effect_class_name - Full class path of a concrete USoundfieldEffectBase subclass, e.g. "/Script/MyAudioPlugin.MySoundfieldEffect"; it is looked up with no outer, so a bare class name does not resolve
//   @returns { submix_path, effect_path, effect_class, chain_length, added, already_present, saved, note? }
// ---------------------------------------------------------------------------
// 2. add_soundfield_effect_to_submix — Resolve effect_class_name -> UClass*, validate
//    !CLASS_Abstract && IsChildOf(USoundfieldEffectBase), NewObject in package, append
//    to SoundfieldEffectChain, save. Honest-gated when 0 concrete subclasses are loadable.
TSharedPtr<FJsonObject> HandleAddSoundfieldEffectToSubmix(const TSharedPtr<FJsonObject>& Params)
{
    FString SubmixPath, EffectClassName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("submix_path"), SubmixPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("effect_class_name"), EffectClassName)) { return Missing; }

    // Pre-check: is there ANY concrete USoundfieldEffectBase subclass loadable?
    // If not, we cannot populate the chain at all — honest-gate BEFORE attempting.
    UClass* AnyConcrete = FindAnyConcreteSoundfieldEffectClass();
    if (!AnyConcrete)
    {
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("submix_path"), SubmixPath);
        ResultObj->SetStringField(TEXT("effect_class"), EffectClassName);
        ResultObj->SetBoolField(TEXT("added"), false);
        ResultObj->SetBoolField(TEXT("already_present"), false);
        ResultObj->SetBoolField(TEXT("saved"), false);
        ResultObj->SetStringField(TEXT("note"), SoundfieldHonestGateNote());
        return mcp_common::MakeOutcomeReply(ResultObj, false,
            TEXT("add_soundfield_effect_to_submix: no concrete soundfield effect class is loaded, so nothing was added (see note)."));
    }

    USoundfieldSubmix* Submix = nullptr;
    if (TSharedPtr<FJsonObject> Error = Soundfield_LoadSubmix(SubmixPath, Submix)) { return Error; }

    // Resolve friendly-name -> concrete class (validates !abstract + IsChildOf).
    FString ResolveError;
    UClass* EffectClass = ResolveConcreteSoundfieldEffectClass(EffectClassName, ResolveError);
    if (!EffectClass)
    {
        return mcp_common::MakeErrorReply(ResolveError);
    }

    // Idempotency: scan existing chain for an instance of the same class.
    USoundfieldEffectBase* ExistingOfSameClass = nullptr;
    for (USoundfieldEffectBase* E : Submix->SoundfieldEffectChain)
    {
        if (E && E->GetClass() == EffectClass)
        {
            ExistingOfSameClass = E;
            break;
        }
    }
    if (ExistingOfSameClass)
    {
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("submix_path"), Submix->GetPathName());
        ResultObj->SetStringField(TEXT("effect_path"), ExistingOfSameClass->GetPathName());
        ResultObj->SetStringField(TEXT("effect_class"), EffectClass->GetName());
        ResultObj->SetBoolField(TEXT("added"), false);
        ResultObj->SetBoolField(TEXT("already_present"), true);
        ResultObj->SetBoolField(TEXT("saved"), false);
        ResultObj->SetNumberField(TEXT("chain_length"), Submix->SoundfieldEffectChain.Num());
        return mcp_common::MakeOkReply(ResultObj);
    }

    // Create the effect asset inside the submix's package so it lives with the submix.
    FScopedTransaction Tx(NSLOCTEXT("UEAgent", "AddSoundfieldEffect", "MCP Add Soundfield Effect"));
    Submix->Modify();

    USoundfieldEffectBase* NewEffect = NewObject<USoundfieldEffectBase>(Submix->GetOutermost(), EffectClass,
        MakeUniqueObjectName(Submix->GetOutermost(), EffectClass),
        RF_Public | RF_Standalone | RF_Transactional);
    if (!NewEffect)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to NewObject<USoundfieldEffectBase>(%s)"), *EffectClassName));
    }
    FAssetRegistryModule::AssetCreated(NewEffect);

    Submix->SoundfieldEffectChain.Add(NewEffect);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Submix->GetOutermost(), Submix, SaveError);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("submix_path"), Submix->GetPathName());
    ResultObj->SetStringField(TEXT("effect_path"), NewEffect->GetPathName());
    ResultObj->SetStringField(TEXT("effect_class"), EffectClass->GetName());
    ResultObj->SetBoolField(TEXT("added"), true);
    ResultObj->SetBoolField(TEXT("already_present"), false);
    ResultObj->SetNumberField(TEXT("chain_length"), Submix->SoundfieldEffectChain.Num());
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("add_soundfield_effect_to_submix"), SaveError);
}

// ---------------------------------------------------------------------------
// MCP:get_soundfield_submix_info - Read a USoundfieldSubmix asset without changing it: its soundfield_encoding_format and resolved_format, encoding_settings {class, path}, and effect_chain entries {index, class, path, settings_class, settings_path} with chain_length. Adds a note when no concrete soundfield effect class is loaded.
//   @param submix_path - Full path to the USoundfieldSubmix (required).
//   @returns { submix_path, soundfield_encoding_format, resolved_format, encoding_settings_class, encoding_settings: { class, path }, effect_chain: [{ index, class, path, settings_class, settings_path }], chain_length, note? }
// ---------------------------------------------------------------------------
// 4. get_soundfield_submix_info — Reader: format, encoding_settings path/class, effect_chain[]
//    with per-effect class + path + (when present) settings class. Always headless.
TSharedPtr<FJsonObject> HandleGetSoundfieldSubmixInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString SubmixPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("submix_path"), SubmixPath)) { return Missing; }
    USoundfieldSubmix* Submix = nullptr;
    if (TSharedPtr<FJsonObject> Error = Soundfield_LoadSubmix(SubmixPath, Submix)) { return Error; }

    const FName ResolvedFormat = Submix->GetSubmixFormat();
    const USoundfieldEncodingSettingsBase* EncodingSettings = Submix->GetEncodingSettings();
    // Engine accessor may return the live field OR transitively-resolved one.
    // Prefer the live field, fall back to the engine accessor.
    const USoundfieldEncodingSettingsBase* EncodingSettingsLive = Submix->EncodingSettings.Get();
    const USoundfieldEncodingSettingsBase* EncodingSettingsEff = EncodingSettingsLive
        ? EncodingSettingsLive : EncodingSettings;

    TSharedPtr<FJsonObject> EncodingSettingsObj = MakeShared<FJsonObject>();
    if (EncodingSettingsEff)
    {
        EncodingSettingsObj->SetStringField(TEXT("class"), EncodingSettingsEff->GetClass()->GetName());
        EncodingSettingsObj->SetStringField(TEXT("path"), EncodingSettingsEff->GetPathName());
    }
    else
    {
        EncodingSettingsObj->SetStringField(TEXT("class"), TEXT(""));
        EncodingSettingsObj->SetStringField(TEXT("path"), TEXT(""));
    }

    TArray<TSharedPtr<FJsonValue>> ChainArr;
    ChainArr.Reserve(Submix->SoundfieldEffectChain.Num());
    int32 Idx = 0;
    for (USoundfieldEffectBase* Effect : Submix->SoundfieldEffectChain)
    {
        ChainArr.Add(MakeShared<FJsonValueObject>(EffectToJson(Idx, Effect)));
        ++Idx;
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("submix_path"), Submix->GetPathName());
    ResultObj->SetStringField(TEXT("soundfield_encoding_format"), Submix->SoundfieldEncodingFormat.ToString());
    ResultObj->SetStringField(TEXT("resolved_format"), ResolvedFormat.ToString());
    ResultObj->SetStringField(TEXT("encoding_settings_class"),
        EncodingSettingsEff ? EncodingSettingsEff->GetClass()->GetName() : TEXT(""));
    ResultObj->SetObjectField(TEXT("encoding_settings"), EncodingSettingsObj);
    ResultObj->SetArrayField(TEXT("effect_chain"), ChainArr);
    ResultObj->SetNumberField(TEXT("chain_length"), Submix->SoundfieldEffectChain.Num());
    // Honest note when 0 concrete subclasses ship — helps the agent understand the empty chain.
    if (!FindAnyConcreteSoundfieldEffectClass())
    {
        ResultObj->SetStringField(TEXT("note"), SoundfieldHonestGateNote());
    }
    return ResultObj;
}

// ---------------------------------------------------------------------------
// MCP:remove_soundfield_effect - Detach the effect at effect_index from a USoundfieldSubmix effect chain (undoable) and save; the effect object stays in the package, an out-of-range index is an error, and a removal succeeds only when the submix reaches disk. Returns removed_effect_class, removed_effect_path and chain_length. With no concrete effect class loaded nothing changes and the call fails, with removed false and a note.
//   @param submix_path - Full path to the USoundfieldSubmix (required).
//   @param effect_index - Zero-based index into SoundfieldEffectChain (required).
//   @returns { submix_path, effect_index, removed_effect_class, removed_effect_path, removed: true, already_absent: false, chain_length, saved, save_error? } -- success only when saved; with no concrete effect class loaded the call fails with { submix_path, effect_index, removed: false, saved: false, note } and the chain is not touched.
// ---------------------------------------------------------------------------
// 5. remove_soundfield_effect — RemoveAt on SoundfieldEffectChain[effect_index] (soft remove:
//    only detaches from chain, leaves the asset on disk for reversibility). Honest-gated
//    when chain empty.
TSharedPtr<FJsonObject> HandleRemoveSoundfieldEffect(const TSharedPtr<FJsonObject>& Params)
{
    FString SubmixPath;
    int32 EffectIndex = -1;
    if (TSharedPtr<FJsonObject> Error = Soundfield_ReadEffectSlot(Params, SubmixPath, EffectIndex)) { return Error; }

    if (!FindAnyConcreteSoundfieldEffectClass())
    {
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("submix_path"), SubmixPath);
        ResultObj->SetNumberField(TEXT("effect_index"), EffectIndex);
        ResultObj->SetBoolField(TEXT("removed"), false);
        ResultObj->SetBoolField(TEXT("saved"), false);
        ResultObj->SetStringField(TEXT("note"), SoundfieldHonestGateNote());
        return mcp_common::MakeOutcomeReply(ResultObj, false,
            TEXT("remove_soundfield_effect: no concrete soundfield effect class is loaded, so the chain was not touched (see note)."));
    }

    USoundfieldSubmix* Submix = nullptr;
    if (TSharedPtr<FJsonObject> Error = Soundfield_LoadSubmix(SubmixPath, Submix)) { return Error; }
    if (!Submix->SoundfieldEffectChain.IsValidIndex(EffectIndex))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("effect_index %d out of range (chain length %d)"),
                EffectIndex, Submix->SoundfieldEffectChain.Num()));
    }

    // Capture the removed effect's identity for the response.
    USoundfieldEffectBase* Removed = Submix->SoundfieldEffectChain[EffectIndex];
    const FString RemovedClass = Removed ? Removed->GetClass()->GetName() : TEXT("");
    const FString RemovedPath = Removed ? Removed->GetPathName() : TEXT("");

    FScopedTransaction Tx(NSLOCTEXT("UEAgent", "RemoveSoundfieldEffect", "MCP Remove Soundfield Effect"));
    Submix->Modify();
    Submix->SoundfieldEffectChain.RemoveAt(EffectIndex);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Submix->GetOutermost(), Submix, SaveError);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("submix_path"), Submix->GetPathName());
    ResultObj->SetNumberField(TEXT("effect_index"), EffectIndex);
    ResultObj->SetStringField(TEXT("removed_effect_class"), RemovedClass);
    ResultObj->SetStringField(TEXT("removed_effect_path"), RemovedPath);
    ResultObj->SetBoolField(TEXT("removed"), true);
    ResultObj->SetBoolField(TEXT("already_absent"), false);
    ResultObj->SetNumberField(TEXT("chain_length"), Submix->SoundfieldEffectChain.Num());
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("remove_soundfield_effect"), SaveError);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_soundfield_effect_to_submix"), &HandleAddSoundfieldEffectToSubmix },
    { TEXT("create_soundfield_submix"), &HandleCreateSoundfieldSubmix },
    { TEXT("get_soundfield_submix_info"), &HandleGetSoundfieldSubmixInfo },
    { TEXT("remove_soundfield_effect"), &HandleRemoveSoundfieldEffect },
};
} // namespace UEAgentSoundfieldCommands
