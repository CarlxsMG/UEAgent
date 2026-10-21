// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// One tool per thing a Level Sequence has settings on -- a section, the sequence itself -- instead of one per setting.
// Each setting is applied by the tool that owns it (its validation, undo step and save), through the bridge's dispatch.
namespace UEAgentSequencerSettingsCommands
{

// MCP-DOMAIN: Timeline

namespace
{
    // The addressing params of a section, copied as given into each step.
    const TCHAR* const SectionAddress[] = {
        TEXT("sequence_path"), TEXT("sequence_name"), TEXT("binding_guid"), TEXT("binding_name_or_guid"),
        TEXT("track_type"), TEXT("track_index"), TEXT("section_index") };

    TSharedPtr<FJsonObject> SectionStepParams(const TSharedPtr<FJsonObject>& Params)
    {
        TSharedPtr<FJsonObject> StepParams = MakeShared<FJsonObject>();
        for (const TCHAR* Key : SectionAddress)
        {
            if (const TSharedPtr<FJsonValue> Value = Params->TryGetField(Key))
            {
                StepParams->SetField(Key, Value);
            }
        }
        return StepParams;
    }
}

// MCP:set_section_properties - Change any settings of ONE section of a Level Sequence in one call: active (muted or not), locked, blend type, completion mode, color tint, row index, easing and pre/post roll. The section is addressed as every section tool addresses it (sequence_path; binding_guid for a track of a binding, or none for a root track such as a shot, a camera cut or an audio track; then track_type or track_index, then section_index); pass only the settings to change. Each is validated and applied by its own step -- unlocking first and locking last, so a locked section can be changed and relocked in one call -- and saved; the call stops at the first setting refused (a blend type the track does not support, muting a locked section), with applied listing those already changed. Read it back with get_section_info. set_section_range moves the section in time; set_audio_section_properties sets an audio section's volume and pitch.
//   @param sequence_path - Asset path of the Level Sequence
//   @param binding_guid - Optional GUID of the binding that owns the track (from get_sequence_tracks); omitted, track_type or track_index pick among the root tracks
//   @param binding_name_or_guid - Optional: the binding by display name instead of binding_guid
//   @param sequence_name - Optional: the sequence by asset name instead of sequence_path
//   @param track_type - Optional track class name (e.g. MovieScene3DTransformTrack) to pick the track
//   @param track_index [integer] - Optional index of the track within the binding (default 0)
//   @param section_index [integer] - Optional index of the section within the track (default 0)
//   @param active [bool] - Optional: false mutes the section, true unmutes it
//   @param locked [bool] - Optional: lock or unlock the section against edits in Sequencer
//   @param blend_type - Optional: Absolute, Additive, Relative or AdditiveFromBase, as the track supports
//   @param completion_mode - Optional: KeepState, RestoreState or ProjectDefault
//   @param color [array] - Optional color tint [r, g, b] or [r, g, b, a], each 0..1
//   @param row_index [integer] - Optional row of the section within its track
//   @param ease_in_frames - Optional manual ease-in length in frames (sets manual easing)
//   @param ease_out_frames - Optional manual ease-out length in frames (sets manual easing)
//   @param pre_roll_frames - Optional pre-roll length in frames
//   @param post_roll_frames - Optional post-roll length in frames
//   @returns { applied: { setting: that setting's reply }, failed?: { label, command, error } (when a setting is refused) }
TSharedPtr<FJsonObject> HandleSetSectionProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    Params->TryGetStringField(TEXT("sequence_path"), SequencePath);
    FString SequenceName;
    Params->TryGetStringField(TEXT("sequence_name"), SequenceName);
    if (SequencePath.IsEmpty() && SequenceName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'sequence_path' (or sequence_name); nothing was changed"));
    }

    TArray<mcp_common::FSubCommand> Steps;
    bool bFlag = false;
    double Number = 0.0;
    FString Text;
    bool bLockAtEnd = false;

    if (Params->TryGetBoolField(TEXT("locked"), bFlag))
    {
        if (bFlag)
        {
            bLockAtEnd = true;
        }
        else
        {
            TSharedPtr<FJsonObject> Unlock = SectionStepParams(Params);
            Unlock->SetBoolField(TEXT("locked"), false);
            Steps.Add({ TEXT("locked"), TEXT("set_section_locked"), Unlock });
        }
    }
    if (Params->TryGetBoolField(TEXT("active"), bFlag))
    {
        TSharedPtr<FJsonObject> Active = SectionStepParams(Params);
        Active->SetBoolField(TEXT("active"), bFlag);
        Steps.Add({ TEXT("active"), TEXT("set_section_active"), Active });
    }
    if (Params->TryGetStringField(TEXT("blend_type"), Text))
    {
        TSharedPtr<FJsonObject> Blend = SectionStepParams(Params);
        Blend->SetStringField(TEXT("blend_type"), Text);
        Steps.Add({ TEXT("blend_type"), TEXT("set_section_blend_type"), Blend });
    }
    if (Params->TryGetStringField(TEXT("completion_mode"), Text))
    {
        TSharedPtr<FJsonObject> Completion = SectionStepParams(Params);
        Completion->SetStringField(TEXT("completion_mode"), Text);
        Steps.Add({ TEXT("completion_mode"), TEXT("set_section_completion_mode"), Completion });
    }
    const TArray<TSharedPtr<FJsonValue>>* Color = nullptr;
    if (Params->TryGetArrayField(TEXT("color"), Color) && Color)
    {
        if (Color->Num() != 3 && Color->Num() != 4)
        {
            return mcp_common::MakeErrorReply(TEXT("color must be [r, g, b] or [r, g, b, a]; nothing was changed"));
        }
        TSharedPtr<FJsonObject> Tint = SectionStepParams(Params);
        const TCHAR* const Channels[] = { TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") };
        for (int32 Index = 0; Index < Color->Num(); ++Index)
        {
            double Channel = 0.0;
            if (!(*Color)[Index].IsValid() || !(*Color)[Index]->TryGetNumber(Channel))
            {
                return mcp_common::MakeErrorReply(TEXT("color must hold numbers; nothing was changed"));
            }
            Tint->SetNumberField(Channels[Index], Channel);
        }
        Steps.Add({ TEXT("color"), TEXT("set_section_color"), Tint });
    }
    if (Params->TryGetNumberField(TEXT("row_index"), Number))
    {
        TSharedPtr<FJsonObject> Row = SectionStepParams(Params);
        Row->SetNumberField(TEXT("row_index"), Number);
        Steps.Add({ TEXT("row_index"), TEXT("set_section_row_index"), Row });
    }
    const bool bEaseIn = Params->HasField(TEXT("ease_in_frames"));
    const bool bEaseOut = Params->HasField(TEXT("ease_out_frames"));
    if (bEaseIn || bEaseOut)
    {
        TSharedPtr<FJsonObject> Easing = SectionStepParams(Params);
        Easing->SetBoolField(TEXT("use_manual"), true);
        if (Params->TryGetNumberField(TEXT("ease_in_frames"), Number))
        {
            Easing->SetNumberField(TEXT("ease_in_frames"), Number);
        }
        if (Params->TryGetNumberField(TEXT("ease_out_frames"), Number))
        {
            Easing->SetNumberField(TEXT("ease_out_frames"), Number);
        }
        Steps.Add({ TEXT("easing"), TEXT("set_section_easing"), Easing });
    }
    const bool bPreRoll = Params->HasField(TEXT("pre_roll_frames"));
    const bool bPostRoll = Params->HasField(TEXT("post_roll_frames"));
    if (bPreRoll || bPostRoll)
    {
        TSharedPtr<FJsonObject> Roll = SectionStepParams(Params);
        if (Params->TryGetNumberField(TEXT("pre_roll_frames"), Number))
        {
            Roll->SetNumberField(TEXT("pre_roll_frames"), Number);
        }
        if (Params->TryGetNumberField(TEXT("post_roll_frames"), Number))
        {
            Roll->SetNumberField(TEXT("post_roll_frames"), Number);
        }
        Steps.Add({ TEXT("pre_post_roll"), TEXT("set_section_pre_post_rollout"), Roll });
    }
    if (bLockAtEnd)
    {
        TSharedPtr<FJsonObject> Lock = SectionStepParams(Params);
        Lock->SetBoolField(TEXT("locked"), true);
        Steps.Add({ TEXT("locked"), TEXT("set_section_locked"), Lock });
    }
    return mcp_common::RunSubCommands(TEXT("set_section_properties"), Steps, MakeShared<FJsonObject>());
}

// MCP:set_sequence_settings - Change any settings of a Level Sequence itself in one call: display rate, playback range (in tick frames), the editor's view and working ranges (in seconds), and the playback-range and marked-frames locks. Pass only the settings to change; each is validated and applied by its own step with its undo step and save, in the order listed here, and the call stops at the first one refused, with applied listing those already changed. The tick resolution is set_sequence_frame_rate, which also moves every key; get_sequence_info reads these back.
//   @param sequence_path - Asset path of the Level Sequence (/Game/...)
//   @param display_rate_fps - Optional display rate in frames per second, above 0
//   @param playback_start_frame [integer] - Optional inclusive start of the playback range, in tick frames; needs playback_end_frame
//   @param playback_end_frame [integer] - Optional exclusive end of the playback range, in tick frames; needs playback_start_frame
//   @param view_start_seconds - Optional start of the editor's view range, in seconds; needs view_end_seconds
//   @param view_end_seconds - Optional end of the editor's view range, in seconds
//   @param working_start_seconds - Optional start of the editor's working range, in seconds; needs working_end_seconds
//   @param working_end_seconds - Optional end of the editor's working range, in seconds
//   @param playback_range_locked [bool] - Optional: lock or unlock the playback range in Sequencer
//   @param marked_frames_locked [bool] - Optional: lock or unlock the marked frames in Sequencer
//   @returns { applied: { setting: that setting's reply }, failed?: { label, command, error } (when a setting is refused) }
TSharedPtr<FJsonObject> HandleSetSequenceSettings(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_path"), SequencePath)) { return Missing; }
    // The range and lock tools find the sequence by asset name.
    FString SequenceName = FPackageName::GetShortName(SequencePath);
    int32 Dot = INDEX_NONE;
    if (SequenceName.FindChar(TEXT('.'), Dot))
    {
        SequenceName.LeftInline(Dot);
    }

    TArray<mcp_common::FSubCommand> Steps;
    double Number = 0.0;
    bool bFlag = false;
    auto ByPath = [&SequencePath]()
    {
        TSharedPtr<FJsonObject> StepParams = MakeShared<FJsonObject>();
        StepParams->SetStringField(TEXT("sequence_path"), SequencePath);
        return StepParams;
    };
    auto ByName = [&SequenceName]()
    {
        TSharedPtr<FJsonObject> StepParams = MakeShared<FJsonObject>();
        StepParams->SetStringField(TEXT("sequence_name"), SequenceName);
        return StepParams;
    };
    auto RangePair = [&Params](const TCHAR* StartKey, const TCHAR* EndKey, double& OutStart, double& OutEnd, FString& OutError)
    {
        const bool bStart = Params->TryGetNumberField(StartKey, OutStart);
        const bool bEnd = Params->TryGetNumberField(EndKey, OutEnd);
        if (bStart != bEnd)
        {
            OutError = FString::Printf(TEXT("%s and %s go together; nothing was changed"), StartKey, EndKey);
        }
        return bStart && bEnd;
    };

    if (Params->TryGetNumberField(TEXT("display_rate_fps"), Number))
    {
        TSharedPtr<FJsonObject> Rate = ByPath();
        Rate->SetNumberField(TEXT("fps"), Number);
        Steps.Add({ TEXT("display_rate_fps"), TEXT("set_sequence_display_rate"), Rate });
    }
    double Start = 0.0;
    double End = 0.0;
    FString PairError;
    if (RangePair(TEXT("playback_start_frame"), TEXT("playback_end_frame"), Start, End, PairError))
    {
        TSharedPtr<FJsonObject> Playback = ByName();
        Playback->SetNumberField(TEXT("start_frame"), Start);
        Playback->SetNumberField(TEXT("end_frame"), End);
        Steps.Add({ TEXT("playback_range"), TEXT("set_sequence_playback_range"), Playback });
    }
    if (RangePair(TEXT("view_start_seconds"), TEXT("view_end_seconds"), Start, End, PairError))
    {
        TSharedPtr<FJsonObject> View = ByPath();
        View->SetNumberField(TEXT("start_seconds"), Start);
        View->SetNumberField(TEXT("end_seconds"), End);
        Steps.Add({ TEXT("view_range"), TEXT("set_sequence_view_range"), View });
    }
    if (RangePair(TEXT("working_start_seconds"), TEXT("working_end_seconds"), Start, End, PairError))
    {
        TSharedPtr<FJsonObject> Working = ByPath();
        Working->SetNumberField(TEXT("start_seconds"), Start);
        Working->SetNumberField(TEXT("end_seconds"), End);
        Steps.Add({ TEXT("working_range"), TEXT("set_sequence_working_range"), Working });
    }
    if (!PairError.IsEmpty())
    {
        return mcp_common::MakeErrorReply(PairError);
    }
    if (Params->TryGetBoolField(TEXT("playback_range_locked"), bFlag))
    {
        TSharedPtr<FJsonObject> Lock = ByName();
        Lock->SetBoolField(TEXT("locked"), bFlag);
        Steps.Add({ TEXT("playback_range_locked"), TEXT("set_playback_range_locked"), Lock });
    }
    if (Params->TryGetBoolField(TEXT("marked_frames_locked"), bFlag))
    {
        TSharedPtr<FJsonObject> Lock = ByName();
        Lock->SetBoolField(TEXT("locked"), bFlag);
        Steps.Add({ TEXT("marked_frames_locked"), TEXT("set_marked_frames_locked"), Lock });
    }
    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("sequence_path"), SequencePath);
    return mcp_common::RunSubCommands(TEXT("set_sequence_settings"), Steps, Reply);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("set_section_properties"), &HandleSetSectionProperties },
    { TEXT("set_sequence_settings"), &HandleSetSequenceSettings },
};
} // namespace UEAgentSequencerSettingsCommands
