#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Asset/AssetCommandHelpers.h"

// Engine subtitles (USubtitleAssetUserData + FSubtitleAssetData — Runtime/Engine module,
// header under Subtitles/; no extra module add needed)
#include "Subtitles/SubtitlesAndClosedCaptionsTypes.h"

// Engine / editor
#include "Editor.h"                          // GEditor
#include "Engine/Engine.h"                   // GEngine
#include "ScopedTransaction.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"

/**
 * Subtitle authoring commands.
 *
 * Backed by USubtitleAssetUserData, whose Subtitles array of FSubtitleAssetData cues is the engine's
 * subtitle authoring surface. The class lives in the Engine module's
 * Subtitles/SubtitlesAndClosedCaptionsTypes.h; the SubtitlesAndClosedCaptions plugin re-exports it for
 * plugin consumers.
 */
namespace UEAgentSubtitlesCommands
{

// MCP-DOMAIN: Audio

namespace
{
    // Map wire-string "use_sound_duration" / "use_duration_property" -> ESubtitleDurationType.
    // Returns false + OutError on unrecognized value.
    bool ParseDurationType(const FString& InStr, ESubtitleDurationType& Out, FString& OutError)
    {
        const FString Lower = InStr.ToLower();
        if (Lower == TEXT("use_sound_duration"))
        {
            Out = ESubtitleDurationType::UseSoundDuration;
            return true;
        }
        if (Lower == TEXT("use_duration_property"))
        {
            Out = ESubtitleDurationType::UseDurationProperty;
            return true;
        }
        OutError = FString::Printf(
            TEXT("Unknown duration_type '%s' (expected 'use_sound_duration' or 'use_duration_property')"),
            *InStr);
        return false;
    }

    // Map wire-string "subtitle" / "closed_caption" / "audio_description" -> ESubtitleType.
    bool ParseSubtitleType(const FString& InStr, ESubtitleType& Out, FString& OutError)
    {
        const FString Lower = InStr.ToLower();
        if (Lower == TEXT("subtitle"))
        {
            Out = ESubtitleType::Subtitle;
            return true;
        }
        if (Lower == TEXT("closed_caption"))
        {
            Out = ESubtitleType::ClosedCaption;
            return true;
        }
        if (Lower == TEXT("audio_description"))
        {
            Out = ESubtitleType::AudioDescription;
            return true;
        }
        OutError = FString::Printf(
            TEXT("Unknown subtitle_type '%s' (expected 'subtitle', 'closed_caption', or 'audio_description')"),
            *InStr);
        return false;
    }

    // Serialize one FSubtitleAssetData to a JSON object. Mirrors the wire shape used by
    // add_subtitle_cue / list_subtitle_cues so the idempotent-read-back is round-trippable.
    TSharedPtr<FJsonObject> CueToJson(int32 Index, const FSubtitleAssetData& Cue)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("index"), Index);
        Obj->SetStringField(TEXT("text"), Cue.Text.ToString());
        Obj->SetNumberField(TEXT("start_offset"), Cue.StartOffset);
        Obj->SetNumberField(TEXT("duration"), Cue.Duration);
        Obj->SetStringField(TEXT("duration_type"), Cue.SubtitleDurationType == ESubtitleDurationType::UseSoundDuration
            ? TEXT("use_sound_duration")
            : TEXT("use_duration_property"));
        Obj->SetNumberField(TEXT("priority"), Cue.Priority);
        Obj->SetStringField(TEXT("subtitle_type"), Cue.SubtitleType == ESubtitleType::Subtitle
            ? TEXT("subtitle")
            : (Cue.SubtitleType == ESubtitleType::ClosedCaption
                ? TEXT("closed_caption")
                : TEXT("audio_description")));
        Obj->SetStringField(TEXT("comment"), Cue.Comment);
        return Obj;
    }

    // The USubtitleAssetUserData at AssetPath: nullptr once OutAsset holds it, otherwise the error reply.
    TSharedPtr<FJsonObject> Subtitles_LoadAsset(const FString& AssetPath, USubtitleAssetUserData*& OutAsset)
    {
        OutAsset = mcp_common::ResolveAsset<USubtitleAssetUserData>(AssetPath);
        if (!OutAsset)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Could not load USubtitleAssetUserData at %s"), *AssetPath));
        }
        return nullptr;
    }

    // Subtitles_LoadAsset, then CueIndex checked against the asset's cues.
    TSharedPtr<FJsonObject> Subtitles_LoadCue(const FString& AssetPath, int32 CueIndex, USubtitleAssetUserData*& OutAsset)
    {
        if (TSharedPtr<FJsonObject> LoadError = Subtitles_LoadAsset(AssetPath, OutAsset))
        {
            return LoadError;
        }
        if (!OutAsset->Subtitles.IsValidIndex(CueIndex))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("cue_index %d out of range (asset has %d cues)"),
                    CueIndex, OutAsset->Subtitles.Num()));
        }
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// create_subtitles_asset
// Creates a USubtitleAssetUserData asset at /<package_path>/<name>. The asset
// is a top-level uasset (USubtitleAssetUserData is UCLASS, not an AssetUserData
// child-of-sound); NewObject<USubtitleAssetUserData>(Package, ...) is the
// canonical direct-create path (no USubtitleFactory needed — and we avoid it
// because it's an editor factory with dialog surface and not headless-safe).
// ---------------------------------------------------------------------------
// MCP:create_subtitles_asset - Creates a USubtitleAssetUserData asset (the UE 5.8 subtitle authoring asset).  Creates a new package under /Game and NewObject<USubtitleAssetUserData> into it, then saves. Idempotent: if the asset already exists, returns it with already_existed: true.
//   @param name - Name of the new subtitles asset.
//   @param package_path - Destination /Game folder (default /Game/MCP).
//   @returns { asset_path: string, success: bool, already_existed: bool, cue_count: int, saved: bool }

TSharedPtr<FJsonObject> HandleCreateSubtitlesAsset(
    const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    FString PackagePath = mcp_common::DefaultAssetFolder();
    Params->TryGetStringField(TEXT("package_path"), PackagePath);
    PackagePath.RemoveFromEnd(TEXT("/"));

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // Idempotent: load and return existing asset (don't re-create — would wipe cues).
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        USubtitleAssetUserData* Existing = mcp_common::ResolveAsset<USubtitleAssetUserData>(FullPath);
        if (Existing)
        {
            TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
            ResultObj->SetStringField(TEXT("asset_path"), Existing->GetPathName());
            ResultObj->SetBoolField(TEXT("success"), true);
            ResultObj->SetBoolField(TEXT("already_existed"), true);
            ResultObj->SetNumberField(TEXT("cue_count"), Existing->Subtitles.Num());
            ResultObj->SetBoolField(TEXT("saved"), true);
            return mcp_common::MakeOkReply(ResultObj);
        }
    }

    FString CreateError;
    USubtitleAssetUserData* Asset = mcp_common::NewAssetInPackage<USubtitleAssetUserData>(
        FullPath, Name, TEXT("Failed to create USubtitleAssetUserData"), CreateError);
    if (!Asset)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    FAssetRegistryModule::AssetCreated(Asset);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Asset->GetOutermost(), Asset, SaveError);
    if (!bSaved)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("SaveAssetPackage failed for '%s': %s"), *FullPath, *SaveError));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_path"), Asset->GetPathName());
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetBoolField(TEXT("already_existed"), false);
    ResultObj->SetNumberField(TEXT("cue_count"), Asset->Subtitles.Num());
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("create_subtitles_asset"), SaveError);
}

// ---------------------------------------------------------------------------
// add_subtitle_cue
// Loads a USubtitleAssetUserData, builds a FSubtitleAssetData, dedupes by
// FSubtitleAssetData::operator== (text+duration+start+priority+type; ignores
// Comment), and either appends or returns the existing cue's index.
// ---------------------------------------------------------------------------
// MCP:add_subtitle_cue - Appends a subtitle cue (FSubtitleAssetData) to a USubtitleAssetUserData.  Builds the cue from the supplied text/timing/priority/type/comment, dedupes by FSubtitleAssetData::operator== (text, duration, start_offset, priority, subtitle_type), and either appends or returns the existing cue's index with already_existed: true.
//   @param asset_path - Content path of the USubtitleAssetUserData asset.
//   @param text - The subtitle text (required).
//   @param start_offset - Initial delay in seconds before the subtitle is visible (default 0).
//   @param duration - Display duration in seconds (default 3).
//   @param duration_type - "use_sound_duration" or "use_duration_property" (default "use_duration_property").
//   @param subtitle_type - "subtitle", "closed_caption", or "audio_description" (default "subtitle").
//   @param priority - Subtitle priority; higher values play instead of lower (default 1).
//   @param comment - Editor-only author comment (default "").
//   @returns { asset_path: string, cue_index: int, text: string, success: bool, already_existed: bool, cue_count: int, saved: bool }

TSharedPtr<FJsonObject> HandleAddSubtitleCue(
    const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }

    FString Text;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("text"), Text)) { return Missing; }

    USubtitleAssetUserData* Asset = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = Subtitles_LoadAsset(AssetPath, Asset))
    {
        return LoadError;
    }

    // Build the candidate cue.
    FSubtitleAssetData Cue;
    Cue.Text = FText::FromString(Text);

    double StartOffsetD = 0.0;
    Params->TryGetNumberField(TEXT("start_offset"), StartOffsetD);
    Cue.StartOffset = static_cast<float>(StartOffsetD);

    double DurationD = 3.0;
    Params->TryGetNumberField(TEXT("duration"), DurationD);
    Cue.Duration = static_cast<float>(DurationD);

    FString DurationTypeStr = TEXT("use_duration_property");
    Params->TryGetStringField(TEXT("duration_type"), DurationTypeStr);
    {
        FString Err;
        if (!ParseDurationType(DurationTypeStr, Cue.SubtitleDurationType, Err))
        {
            return mcp_common::MakeErrorReply(Err);
        }
    }

    double PriorityD = 1.0;
    Params->TryGetNumberField(TEXT("priority"), PriorityD);
    Cue.Priority = static_cast<float>(PriorityD);

    FString SubtitleTypeStr = TEXT("subtitle");
    Params->TryGetStringField(TEXT("subtitle_type"), SubtitleTypeStr);
    {
        FString Err;
        if (!ParseSubtitleType(SubtitleTypeStr, Cue.SubtitleType, Err))
        {
            return mcp_common::MakeErrorReply(Err);
        }
    }

    FString CommentStr;
    Params->TryGetStringField(TEXT("comment"), CommentStr);
    Cue.Comment = CommentStr;

    // Idempotent dedupe by operator== (text + duration + start_offset + priority + type).
    int32 ExistingIndex = INDEX_NONE;
    for (int32 i = 0; i < Asset->Subtitles.Num(); ++i)
    {
        if (Asset->Subtitles[i] == Cue)
        {
            ExistingIndex = i;
            break;
        }
    }
    if (ExistingIndex != INDEX_NONE)
    {
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("asset_path"), Asset->GetPathName());
        ResultObj->SetNumberField(TEXT("cue_index"), ExistingIndex);
        ResultObj->SetStringField(TEXT("text"), Cue.Text.ToString());
        ResultObj->SetBoolField(TEXT("success"), true);
        ResultObj->SetBoolField(TEXT("already_existed"), true);
        ResultObj->SetNumberField(TEXT("cue_count"), Asset->Subtitles.Num());
        ResultObj->SetBoolField(TEXT("saved"), true);
        return mcp_common::MakeOkReply(ResultObj);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddSubtitleCue", "Add Subtitle Cue"));
    Asset->Modify();

    Asset->Subtitles.Add(Cue);
    const int32 NewIndex = Asset->Subtitles.Num() - 1;

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Asset->GetOutermost(), Asset, SaveError);
    if (!bSaved)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("SaveAssetPackage failed: %s"), *SaveError));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_path"), Asset->GetPathName());
    ResultObj->SetNumberField(TEXT("cue_index"), NewIndex);
    ResultObj->SetStringField(TEXT("text"), Cue.Text.ToString());
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetBoolField(TEXT("already_existed"), false);
    ResultObj->SetNumberField(TEXT("cue_count"), Asset->Subtitles.Num());
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("add_subtitle_cue"), SaveError);
}

// ---------------------------------------------------------------------------
// set_subtitle_cue_text
// Sets Subtitles[cue_index].Text on an existing USubtitleAssetUserData.
// ---------------------------------------------------------------------------
// MCP:set_subtitle_cue_text - Sets the text on an existing subtitle cue.  Loads the USubtitleAssetUserData, verifies cue_index is in range, sets Subtitles[cue_index].Text, and saves.
//   @param asset_path - Content path of the USubtitleAssetUserData asset.
//   @param cue_index - Zero-based index into the Subtitles array (required).
//   @param text - New subtitle text (required).
//   @returns { asset_path: string, cue_index: int, text: string, success: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetSubtitleCueText(
    const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }

    double CueIndexD = -1.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("cue_index"), CueIndexD)) { return Missing; }
    const int32 CueIndex = static_cast<int32>(CueIndexD);

    FString Text;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("text"), Text)) { return Missing; }

    USubtitleAssetUserData* Asset = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = Subtitles_LoadCue(AssetPath, CueIndex, Asset))
    {
        return LoadError;
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetSubtitleCueText", "Set Subtitle Cue Text"));
    Asset->Modify();

    Asset->Subtitles[CueIndex].Text = FText::FromString(Text);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Asset->GetOutermost(), Asset, SaveError);
    if (!bSaved)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("SaveAssetPackage failed: %s"), *SaveError));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_path"), Asset->GetPathName());
    ResultObj->SetNumberField(TEXT("cue_index"), CueIndex);
    ResultObj->SetStringField(TEXT("text"), Text);
    ResultObj->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_subtitle_cue_text"), SaveError);
}

// ---------------------------------------------------------------------------
// set_subtitle_cue_timing
// Patches StartOffset / Duration / SubtitleDurationType on Subtitles[cue_index].
// All three fields are optional; only present fields are applied. The asset is
// saved even if nothing changed (to keep idempotent "set to X" semantics).
// ---------------------------------------------------------------------------
// MCP:set_subtitle_cue_timing - Updates the timing fields on an existing subtitle cue.  Loads the USubtitleAssetUserData, applies any combination of start_offset / duration / duration_type to Subtitles[cue_index], and saves. Fields not provided are left untouched.
//   @param asset_path - Content path of the USubtitleAssetUserData asset.
//   @param cue_index - Zero-based index into the Subtitles array (required).
//   @param start_offset - Optional new StartOffset (seconds).
//   @param duration - Optional new Duration (seconds).
//   @param duration_type - Optional new SubtitleDurationType ("use_sound_duration" or "use_duration_property").
//   @returns { asset_path: string, cue_index: int, success: bool, applied_start_offset: bool, applied_duration: bool, applied_duration_type: bool, start_offset: float, duration: float, duration_type: string, saved: bool }

TSharedPtr<FJsonObject> HandleSetSubtitleCueTiming(
    const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }

    double CueIndexD = -1.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("cue_index"), CueIndexD)) { return Missing; }
    const int32 CueIndex = static_cast<int32>(CueIndexD);

    USubtitleAssetUserData* Asset = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = Subtitles_LoadCue(AssetPath, CueIndex, Asset))
    {
        return LoadError;
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetSubtitleCueTiming", "Set Subtitle Cue Timing"));
    Asset->Modify();

    bool bAppliedStartOffset = false;
    double StartOffsetD = 0.0;
    if (Params->TryGetNumberField(TEXT("start_offset"), StartOffsetD))
    {
        Asset->Subtitles[CueIndex].StartOffset = static_cast<float>(StartOffsetD);
        bAppliedStartOffset = true;
    }

    bool bAppliedDuration = false;
    double DurationD = 0.0;
    if (Params->TryGetNumberField(TEXT("duration"), DurationD))
    {
        Asset->Subtitles[CueIndex].Duration = static_cast<float>(DurationD);
        bAppliedDuration = true;
    }

    bool bAppliedDurationType = false;
    FString DurationTypeStr;
    if (Params->TryGetStringField(TEXT("duration_type"), DurationTypeStr) && !DurationTypeStr.IsEmpty())
    {
        ESubtitleDurationType Parsed;
        FString Err;
        if (!ParseDurationType(DurationTypeStr, Parsed, Err))
        {
            return mcp_common::MakeErrorReply(Err);
        }
        Asset->Subtitles[CueIndex].SubtitleDurationType = Parsed;
        bAppliedDurationType = true;
    }

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Asset->GetOutermost(), Asset, SaveError);
    if (!bSaved)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("SaveAssetPackage failed: %s"), *SaveError));
    }

    const FSubtitleAssetData& Cue = Asset->Subtitles[CueIndex];

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_path"), Asset->GetPathName());
    ResultObj->SetNumberField(TEXT("cue_index"), CueIndex);
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetBoolField(TEXT("applied_start_offset"), bAppliedStartOffset);
    ResultObj->SetBoolField(TEXT("applied_duration"), bAppliedDuration);
    ResultObj->SetBoolField(TEXT("applied_duration_type"), bAppliedDurationType);
    ResultObj->SetNumberField(TEXT("start_offset"), Cue.StartOffset);
    ResultObj->SetNumberField(TEXT("duration"), Cue.Duration);
    ResultObj->SetStringField(TEXT("duration_type"), Cue.SubtitleDurationType == ESubtitleDurationType::UseSoundDuration
        ? TEXT("use_sound_duration")
        : TEXT("use_duration_property"));
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_subtitle_cue_timing"), SaveError);
}

// ---------------------------------------------------------------------------
// list_subtitle_cues
// Read-only. Walks Subtitles and returns JSON shape with cue_count, max_duration
// (from USubtitleAssetUserData::GetMaximumDuration — Duration + StartOffset),
// and per-cue fields matching add_subtitle_cue's wire shape.
// ---------------------------------------------------------------------------
// MCP:list_subtitle_cues - Lists all subtitle cues on a USubtitleAssetUserData.  Read-only walk of the Subtitles array, returning cue_count, max_duration (from USubtitleAssetUserData::GetMaximumDuration), and per-cue fields (index, text, start_offset, duration, duration_type, priority, subtitle_type, comment).
//   @param asset_path - Content path of the USubtitleAssetUserData asset.
//   @returns { asset_path: string, success: bool, count: int, max_duration: float, cues: [{index, text, start_offset, duration, duration_type, priority, subtitle_type, comment}] }

TSharedPtr<FJsonObject> HandleListSubtitleCues(
    const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }

    USubtitleAssetUserData* Asset = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = Subtitles_LoadAsset(AssetPath, Asset))
    {
        return LoadError;
    }

    TArray<TSharedPtr<FJsonValue>> CuesArr;
    CuesArr.Reserve(Asset->Subtitles.Num());
    for (int32 i = 0; i < Asset->Subtitles.Num(); ++i)
    {
        CuesArr.Add(MakeShared<FJsonValueObject>(CueToJson(i, Asset->Subtitles[i])));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_path"), Asset->GetPathName());
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetNumberField(TEXT("count"), Asset->Subtitles.Num());
    ResultObj->SetNumberField(TEXT("max_duration"), Asset->GetMaximumDuration());
    ResultObj->SetArrayField(TEXT("cues"), CuesArr);
    return mcp_common::MakeOkReply(ResultObj);
}

// ---------------------------------------------------------------------------
// Typed delete cue + typed delete asset.
// ---------------------------------------------------------------------------

// MCP:remove_subtitle_cue - Remove a single FSubtitleAssetData cue by index. Inverse of add_subtitle_cue; use remove_subtitles_asset to destroy the whole asset instead. Idempotent: an out-of-range index returns removed:false + note, not an error.
//   @param asset_path - /Game path to the USubtitleAssetUserData.
//   @param cue_index - 0-based index into Subtitles[].
//   @returns { asset_path, cue_index, removed, previous_count, new_count }

TSharedPtr<FJsonObject> HandleRemoveSubtitleCue(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }
    int32 CueIndex = INDEX_NONE;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("cue_index"), CueIndex)) { return Missing; }

    USubtitleAssetUserData* Asset = mcp_common::ResolveAsset<USubtitleAssetUserData>(AssetPath);
    if (!Asset)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("remove_subtitle_cue: could not load USubtitleAssetUserData at %s"), *AssetPath));
    }
    if (!Asset->Subtitles.IsValidIndex(CueIndex))
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("asset_path"), Asset->GetPathName());
        Resp->SetNumberField(TEXT("cue_index"), CueIndex);
        Resp->SetBoolField(TEXT("removed"), false);
        Resp->SetNumberField(TEXT("previous_count"), Asset->Subtitles.Num());
        Resp->SetNumberField(TEXT("new_count"), Asset->Subtitles.Num());
        Resp->SetStringField(TEXT("note"),
            FString::Printf(TEXT("cue_index %d out of range [0, %d)"), CueIndex, Asset->Subtitles.Num()));
        return mcp_common::MakeOkReply(Resp);
    }

    Asset->Modify();
    Asset->Subtitles.RemoveAt(CueIndex);
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Asset->GetPackage(), Asset, SaveError);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Asset->GetPathName());
    Resp->SetNumberField(TEXT("cue_index"), CueIndex);
    Resp->SetBoolField(TEXT("removed"), true);
    Resp->SetNumberField(TEXT("previous_count"), Asset->Subtitles.Num() + 1);
    Resp->SetNumberField(TEXT("new_count"), Asset->Subtitles.Num());
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("remove_subtitle_cue"), SaveError);
}

// MCP:remove_subtitles_asset - Destroy a USubtitleAssetUserData asset (mirror of create_subtitles_asset). Typed-delete: it loads the asset and REFUSES to delete it if it is not a USubtitleAssetUserData, so it cannot wipe the wrong asset the way the generic delete_asset can. Idempotent on missing assets.
//   @param asset_path - Full /Game/... asset path of the subtitles asset.
//   @returns { asset_path, deleted, already_existed, asset_class?, note? }
//   NOTE (impl): delegates the actual delete to UEditorAssetLibrary::DeleteAsset once the type check passes.

TSharedPtr<FJsonObject> HandleRemoveSubtitlesAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }
    if (!UEditorAssetLibrary::DoesAssetExist(AssetPath))
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("asset_path"), AssetPath);
        Resp->SetBoolField(TEXT("deleted"), false);
        Resp->SetBoolField(TEXT("already_existed"), false);
        Resp->SetStringField(TEXT("note"), TEXT("asset_path does not exist; nothing to remove"));
        return mcp_common::MakeOkReply(Resp);
    }
    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("remove_subtitles_asset: could not load asset at %s"), *AssetPath));
    }
    if (!Asset->IsA<USubtitleAssetUserData>())
    {
        return mcp_common::MakeOkReply(AssetCommandHelpers::WrongTypeRefusal(AssetPath, *Asset, TEXT("USubtitleAssetUserData")));
    }

    const FString AssetClassName = Asset->GetClass()->GetName();
    const bool bDeleted = UEditorAssetLibrary::DeleteAsset(AssetPath);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), AssetPath);
    Resp->SetStringField(TEXT("asset_class"), AssetClassName);
    Resp->SetBoolField(TEXT("deleted"), bDeleted);
    Resp->SetBoolField(TEXT("already_existed"), true);
    return mcp_common::MakeOkReply(Resp);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_subtitle_cue"), &HandleAddSubtitleCue },
    { TEXT("create_subtitles_asset"), &HandleCreateSubtitlesAsset },
    { TEXT("list_subtitle_cues"), &HandleListSubtitleCues },
    { TEXT("remove_subtitle_cue"), &HandleRemoveSubtitleCue },
    { TEXT("remove_subtitles_asset"), &HandleRemoveSubtitlesAsset },
    { TEXT("set_subtitle_cue_text"), &HandleSetSubtitleCueText },
    { TEXT("set_subtitle_cue_timing"), &HandleSetSubtitleCueTiming },
};
} // namespace UEAgentSubtitlesCommands
