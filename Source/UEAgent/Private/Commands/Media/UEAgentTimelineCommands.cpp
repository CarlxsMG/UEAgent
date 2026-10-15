#include "Mcp/McpCommandFamilies.h"
#include "Commands/Media/LevelSequenceUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "EditorAssetLibrary.h"
#include "FileHelpers.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"             // IFileManager::FileExists (delete_level_sequence: is the file still on disk)
// Level sequence asset model: the movie scene, its tracks and its object bindings
#include "MovieScene.h"
#include "MovieSceneTrack.h"
#include "MovieSceneBinding.h"
// Runtime playback (LevelSequence module): the player and the actor that hosts it
#include "LevelSequencePlayer.h"
#include "LevelSequenceActor.h"
#include "MovieSceneSequencePlayer.h"
#include "MovieSceneSequencePlaybackSettings.h"
#include "Engine/Level.h"
// Driving the Sequencer editor from code (LevelSequenceEditor module)
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "MovieSceneTimeUnit.h"
#include "MovieSceneTimeHelpers.h"   // UE::MovieScene::TimeHelpers::MigrateFrameTimes
#include "ScopedTransaction.h"

/**
 * Level Sequence commands.
 *
 * - Assets: create, list and delete sequences, and read or change their playback
 *   length and frame rate.
 * - Runtime: start a ULevelSequencePlayer in a running PIE or game world, then pause,
 *   stop, seek or query it through the player_key handed back when it started.
 * - Editor: inspect a sequence's timeline and tracks, and drive the open Sequencer
 *   (play, pause, move the playhead).
 */
namespace UEAgentTimelineCommands
{

// MCP-DOMAIN: Timeline

// Players started by play_level_sequence_runtime and the actors that host them, both keyed by the
// player_key returned to the caller. Held weakly: ending PIE destroys them.
static TMap<FString, TWeakObjectPtr<ULevelSequencePlayer>> RuntimePlayersByKey;
static TMap<FString, TWeakObjectPtr<ALevelSequenceActor>> RuntimeActorsByKey;

// ---------------------------------------------------------------------------
// Helpers shared by the timeline commands: sequence lookup through the asset
// registry, asset creation, the Sequencer editor, runtime players and the
// stock error replies.
// ---------------------------------------------------------------------------
namespace UEAgentTimelineImpl
{

    TSharedPtr<FJsonObject> SequenceNotFound(const FString& SequenceName)
    {
        return mcp_common::MakeErrorReply(TEXT("Level sequence '") + SequenceName + TEXT("' not found"));
    }

    // Reads the sequence_name the editor-timeline commands need; the reply that words that need when it is missing
    // or empty.
    TSharedPtr<FJsonObject> ReadTimelineTarget(const TSharedPtr<FJsonObject>& Params, FString& OutSequenceName)
    {
        if (!Params->TryGetStringField(TEXT("sequence_name"), OutSequenceName) || OutSequenceName.IsEmpty())
        {
            return mcp_common::MakeErrorReply(
                TEXT("Missing required parameter: sequence_name (Sequencer timeline command needs a target level sequence)"));
        }
        return nullptr;
    }

    // A sequence found by name together with its movie scene. Error, when set, is the
    // reply the handler returns unchanged.
    struct FResolvedSequence
    {
        ULevelSequence* Sequence = nullptr;
        UMovieScene* MovieScene = nullptr;
        TSharedPtr<FJsonObject> Error;
    };

    // Looks SequenceName up. With NoSceneError, a sequence that has no movie scene is a
    // failure as well, reported with that text (commands word it differently); without it,
    // MovieScene may come back null.
    FResolvedSequence ResolveSequence(const FString& SequenceName, const TCHAR* NoSceneError = nullptr)
    {
        FResolvedSequence Found;
        Found.Sequence = LevelSequenceUtils::FindByName(SequenceName);
        if (Found.Sequence == nullptr)
        {
            Found.Error = SequenceNotFound(SequenceName);
            return Found;
        }

        Found.MovieScene = Found.Sequence->GetMovieScene();
        if (Found.MovieScene == nullptr && NoSceneError != nullptr)
        {
            Found.Error = mcp_common::MakeErrorReply(NoSceneError);
        }
        return Found;
    }

    // Returns the sequence asset at PackagePath/SequenceName, creating it when needed.
    // Creation follows the editor's level-sequence factory (package, NewObject, then
    // Initialize(), which is what builds the inner UMovieScene; creating the asset through
    // IAssetTools without a factory skips that step). An asset that already exists is
    // loaded and returned instead, which makes the call idempotent; bOutCreated tells the two apart.
    ULevelSequence* CreateOrLoadLevelSequence(const FString& SequenceName, const FString& PackagePath, FString& OutError,
        bool& bOutCreated)
    {
        bOutCreated = false;
        FString Folder = PackagePath;
        Folder.RemoveFromEnd(TEXT("/"));
        const FString AssetPath = Folder + TEXT("/") + SequenceName;
        OutError = mcp_common::InvalidPackageName(AssetPath);
        if (!OutError.IsEmpty())
        {
            return nullptr;
        }

        if (UEditorAssetLibrary::DoesAssetExist(AssetPath))
        {
            if (ULevelSequence* Existing = LoadObject<ULevelSequence>(nullptr, *AssetPath))
            {
                return Existing;
            }
        }

        OutError = mcp_common::AssetNameConflict(AssetPath, SequenceName, ULevelSequence::StaticClass());
        if (!OutError.IsEmpty())
        {
            return nullptr;
        }
        UPackage* Package = CreatePackage(*AssetPath);
        if (Package == nullptr)
        {
            return nullptr;
        }
        Package->FullyLoad();

        ULevelSequence* Created = NewObject<ULevelSequence>(Package, *SequenceName, RF_Public | RF_Standalone | RF_Transactional);
        if (Created == nullptr)
        {
            return nullptr;
        }
        Created->Initialize();
        if (Created->GetMovieScene() == nullptr)
        {
            return nullptr;
        }

        // Without this the registry would not list the new asset (nor save it as one).
        FAssetRegistryModule::AssetCreated(Created);
        bOutCreated = true;
        return Created;
    }

    // Optional settings of create_level_sequence, each with the default used when the
    // request leaves it out.
    struct FNewSequenceSettings
    {
        FString PackagePath = TEXT("/Game/Sequences");
        double DurationSeconds = 10.0;
        double FramesPerSecond = 30.0;
    };

    FNewSequenceSettings ReadNewSequenceSettings(const TSharedPtr<FJsonObject>& Params)
    {
        FNewSequenceSettings Settings;
        Params->TryGetStringField(TEXT("package_path"), Settings.PackagePath);
        Params->TryGetNumberField(TEXT("duration"), Settings.DurationSeconds);
        Params->TryGetNumberField(TEXT("frame_rate"), Settings.FramesPerSecond);
        return Settings;
    }

    // Seconds spanned by a frame range at Rate, measured as upper bound minus lower bound.
    double RangeSeconds(const TRange<FFrameNumber>& Range, const FFrameRate& Rate)
    {
        return (Range.GetUpperBoundValue() - Range.GetLowerBoundValue()).Value / Rate.AsDecimal();
    }

    // Finds SequenceName and makes it the sequence the Sequencer editor shows, opening it when
    // another (or none) is open. The error reply when it does not exist or cannot be shown.
    TSharedPtr<FJsonObject> FocusSequencerOn(const FString& SequenceName)
    {
        const FResolvedSequence Target = ResolveSequence(SequenceName);
        if (Target.Error.IsValid())
        {
            return Target.Error;
        }
        if (ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence() == Target.Sequence)
        {
            return nullptr;
        }

        const bool bOpened = ULevelSequenceEditorBlueprintLibrary::OpenLevelSequence(Target.Sequence);
        if (bOpened && ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence() == Target.Sequence)
        {
            return nullptr;
        }
        return mcp_common::MakeErrorReply(
            TEXT("Could not open the level sequence in the Sequencer editor (no editor Sequencer available in this context)"));
    }

    // The running world a runtime player may be created in, or null when there is none.
    //
    // The `world` param selects it. Empty or 'game' means 'pie', the
    // default; OutSelector receives the lower-cased selector that was used. The canonical
    // resolver tells server, client and pie:N instances apart, which matters because a
    // cinematic started in the wrong PIE instance plays for the other player. A standalone
    // game world (-game process) is not something the resolver returns, so the active
    // world is accepted when it is one. Editor worlds are never returned: they have no
    // movie-scene tick manager, and playing a sequence there asserts.
    UWorld* ResolveRuntimePlaybackWorld(const TSharedPtr<FJsonObject>& Params, FString& OutSelector)
    {
        FString IgnoredReason;
        UWorld* World = mcp_common::ResolveWorldSelector(Params, TEXT("pie"), OutSelector, IgnoredReason);

        if (World == nullptr)
        {
            World = mcp_common::ActiveGameWorld();
        }
        if (World != nullptr && World->WorldType != EWorldType::PIE && World->WorldType != EWorldType::Game)
        {
            return nullptr;
        }
        return World;
    }

    // Outcome of looking up a runtime player by the key play_level_sequence_runtime returned.
    struct FRuntimePlayerLookup
    {
        ULevelSequencePlayer* Player = nullptr;
        TSharedPtr<FJsonObject> Error;
    };

    FRuntimePlayerLookup LookUpRuntimePlayer(
        const TMap<FString, TWeakObjectPtr<ULevelSequencePlayer>>& Players, const FString& PlayerKey)
    {
        FRuntimePlayerLookup Lookup;
        const TWeakObjectPtr<ULevelSequencePlayer>* Entry = Players.Find(PlayerKey);
        if (Entry == nullptr || !Entry->IsValid())
        {
            Lookup.Error = mcp_common::MakeErrorReply(TEXT("Player not found. Use play_level_sequence_runtime first."));
            return Lookup;
        }

        Lookup.Player = Entry->Get();
        if (Lookup.Player == nullptr)
        {
            Lookup.Error = mcp_common::MakeErrorReply(TEXT("Player is invalid. It may have been destroyed."));
        }
        return Lookup;
    }

    // Paused is checked first, then playing; any other state reads as stopped.
    const TCHAR* PlaybackStatusName(bool bPaused, bool bPlaying)
    {
        if (bPaused)
        {
            return TEXT("Paused");
        }
        return bPlaying ? TEXT("Playing") : TEXT("Stopped");
    }

    // One track as JSON; the track_name field is only written when asked for.
    TSharedPtr<FJsonObject> DescribeTrack(const UMovieSceneTrack& Track, bool bIncludeTrackName)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("display_name"), Track.GetDisplayName().ToString());
        if (bIncludeTrackName)
        {
            Entry->SetStringField(TEXT("track_name"), Track.GetTrackName().ToString());
        }
        Entry->SetStringField(TEXT("class"), Track.GetClass()->GetName());
        Entry->SetNumberField(TEXT("section_count"), Track.GetAllSections().Num());
        return Entry;
    }
}

// --- Level Sequence assets ----------------------------------------------------------

// MCP:create_level_sequence - Creates a Level Sequence asset at package_path/sequence_name and saves it, with playback range [0, duration) and frame_rate, truncated to whole fps, as both tick resolution and display rate; the end frame is duration times that whole rate, to the nearest frame. If that asset already exists it is loaded and those settings are overwritten, its keys, sections and ranges migrated to the new rate so nothing moves in time; sequence_created tells a new asset from an existing one. Not undoable; another asset class at the path, or a duration shorter than one frame, is an error. To retime an existing sequence use set_sequence_frame_rate or set_sequence_duration; list sequences with get_level_sequences.
//   @param sequence_name - Name of the new sequence to create
//   @param package_path - Target content path, defaults to /Game/Sequences
//   @param duration - Playback duration in seconds, above 0 and at most 3600, default 10
//   @param frame_rate - Frame rate in fps, at least 1 and at most 120, truncated to whole fps, default 30
//   @returns { sequence_name: string, package_path: string, duration: number, frame_rate: number, sequence_created: bool (false when the asset already existed), saved: bool, asset_path: string, playback_range_set: bool, tick_rate_set: bool, actual_start_frame: int, actual_end_frame: int }
TSharedPtr<FJsonObject> HandleCreateLevelSequence(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentTimelineImpl;

    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const Impl::FNewSequenceSettings Settings = Impl::ReadNewSequenceSettings(Params);
    const FString& PackagePath = Settings.PackagePath;
    const double Duration = Settings.DurationSeconds;
    const double FrameRate = Settings.FramesPerSecond;

    if (SequenceName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("sequence_name cannot be empty"));
    }
    if (Duration <= 0.0 || Duration > 3600.0)
    {
        return mcp_common::MakeErrorReply(TEXT("Duration must be between 0.0 and 3600.0 seconds"));
    }
    // The rate is truncated to whole fps, so anything below 1 would become a 0/1 rate, which is invalid.
    if (FrameRate < 1.0 || FrameRate > 120.0)
    {
        return mcp_common::MakeErrorReply(TEXT("Frame rate must be between 1 and 120 fps (it is truncated to whole fps)"));
    }

    // The whole-number fps becomes both the tick resolution and the display rate, so one
    // tick is one frame and the last frame is duration * that whole rate, to the nearest frame.
    // Both are settled before the asset is touched, so a refusal writes nothing.
    const int32 WholeFps = static_cast<int32>(FrameRate);
    const FFrameRate Rate(WholeFps, 1);
    const FFrameNumber FirstFrame(0);
    const FFrameNumber LastFrame(FMath::RoundToInt32(Duration * WholeFps));
    if (LastFrame.Value <= 0)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Duration %g s is shorter than one frame at %d fps"), Duration, WholeFps));
    }

    FString CreateError;
    bool bCreated = false;
    ULevelSequence* Sequence = Impl::CreateOrLoadLevelSequence(SequenceName, PackagePath, CreateError, bCreated);
    if (Sequence == nullptr)
    {
        return mcp_common::MakeErrorReply(CreateError.IsEmpty() ? FString(TEXT("Failed to create level sequence")) : CreateError);
    }
    UMovieScene* Scene = Sequence->GetMovieScene();
    if (Scene == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to get movie scene from level sequence"));
    }

    // The resolution goes in first so that the range below is expressed in it. A new sequence has no
    // keys, so it takes the rate directly; an existing one has every key, section and range rescaled
    // to the new resolution (as set_sequence_frame_rate does), so nothing moves in time.
    if (bCreated)
    {
        Scene->SetTickResolutionDirectly(Rate);
    }
    else if (Scene->GetTickResolution() != Rate)
    {
        UE::MovieScene::TimeHelpers::MigrateFrameTimes(Scene->GetTickResolution(), Rate, Scene);
    }
    Scene->SetDisplayRate(Rate);
    Scene->SetPlaybackRange(TRange<FFrameNumber>(FirstFrame, LastFrame));

    // Read the scene back rather than trusting the setters.
    const TRange<FFrameNumber> StoredRange = Scene->GetPlaybackRange();
    const FFrameRate StoredRate = Scene->GetTickResolution();
    const bool bRangeMatches = StoredRange.GetLowerBoundValue() == FirstFrame && StoredRange.GetUpperBoundValue() == LastFrame;
    const bool bRateMatches = StoredRate.Numerator == WholeFps && StoredRate.Denominator == 1;

    Sequence->MarkPackageDirty();
    const bool bSaved = mcp_common::SaveAssetPackage(Sequence->GetOutermost(), Sequence);

    TSharedPtr<FJsonObject> Report = LevelSequenceUtils::NewNamePayload(SequenceName);
    Report->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
    Report->SetStringField(TEXT("package_path"), PackagePath);
    Report->SetNumberField(TEXT("duration"), Duration);
    Report->SetNumberField(TEXT("frame_rate"), FrameRate);
    Report->SetBoolField(TEXT("sequence_created"), bCreated);
    Report->SetBoolField(TEXT("playback_range_set"), bRangeMatches);
    Report->SetBoolField(TEXT("tick_rate_set"), bRateMatches);
    Report->SetNumberField(TEXT("actual_start_frame"), StoredRange.GetLowerBoundValue().Value);
    Report->SetNumberField(TEXT("actual_end_frame"), StoredRange.GetUpperBoundValue().Value);

    return mcp_common::MakeSavedReply(Report, bSaved, TEXT("create_level_sequence"));
}

// MCP:get_level_sequences - List all Level Sequences in the project, optionally filtered by package path
//   @param package_path - Filter to a long package path, defaults to /Game
//   @param max_results - Maximum number of sequences to return, 1 to 10000, default 1000
//   @returns { sequences: [object], total_count: int, max_results: int, truncated: bool }
TSharedPtr<FJsonObject> HandleGetLevelSequences(const TSharedPtr<FJsonObject>& Params)
{
    // The cap is checked as soon as it is read; the folder filter cannot make it valid.
    int32 MaxResults = 1000;
    Params->TryGetNumberField(TEXT("max_results"), MaxResults);
    if (MaxResults <= 0 || MaxResults > 10000)
    {
        return mcp_common::MakeErrorReply(TEXT("max_results must be between 1 and 10000"));
    }
    FString PackagePath = TEXT("/Game");
    Params->TryGetStringField(TEXT("package_path"), PackagePath);

    // Registry metadata only. At most MaxResults entries are reported, in registry order.
    const TArray<FAssetData> Found = LevelSequenceUtils::ListUnder(FName(*PackagePath));
    const int32 Reported = FMath::Min(Found.Num(), MaxResults);

    TArray<TSharedPtr<FJsonValue>> Listed;
    Listed.Reserve(Reported);
    for (int32 Index = 0; Index < Reported; ++Index)
    {
        const FAssetData& Asset = Found[Index];
        const FString ObjectPath = Asset.GetObjectPathString();

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
        Entry->SetStringField(TEXT("package_path"), Asset.PackageName.ToString());
        Entry->SetStringField(TEXT("asset_path"), ObjectPath);
        Entry->SetStringField(TEXT("class"), Asset.AssetClassPath.ToString());
        Entry->SetBoolField(TEXT("is_valid"), Asset.IsValid());
        // Same value as asset_path; kept because callers already read it.
        Entry->SetStringField(TEXT("file_path"), ObjectPath);
        Listed.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetArrayField(TEXT("sequences"), Listed);
    Report->SetNumberField(TEXT("total_count"), Reported);
    Report->SetNumberField(TEXT("max_results"), MaxResults);
    Report->SetBoolField(TEXT("truncated"), Found.Num() > Reported);
    return mcp_common::MakeOkReply(Report);
}

// MCP:delete_level_sequence - Deletes a Level Sequence asset permanently, choosing the first sequence under /Game whose asset name matches sequence_name (case-insensitive). Succeeds only when actually_deleted: the delete reported success, the asset is no longer registered and its file is gone from disk (a read-only file, or one another process holds open, stays behind and fails the call).
//   @param sequence_name - Asset name of the Level Sequence to delete, not its path (e.g. "LS_Intro")
//   @returns { sequence_name: string, asset_path: string, delete_attempted: bool, asset_still_exists: bool, file_still_exists: bool, actually_deleted: bool }
TSharedPtr<FJsonObject> HandleDeleteLevelSequence(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentTimelineImpl;

    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (SequenceName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("sequence_name cannot be empty"));
    }

    const Impl::FResolvedSequence Target = Impl::ResolveSequence(SequenceName);
    if (Target.Error.IsValid())
    {
        return Target.Error;
    }

    // Delete through the editor asset library, then ask again whether the asset exists:
    // only a delete that reported success AND left nothing behind is actually_deleted. The
    // registry forgets the asset even when its file could not be removed, so the file on disk
    // is checked too: a file left behind brings the sequence back when the editor restarts. It is
    // asked of the file system, not of FPackageName::DoesPackageExist, which answers from the
    // registry first and can still list a package whose file was just deleted.
    const FString AssetPath = Target.Sequence->GetPathName();
    FString PackageFile;
    mcp_common::FindPackageFileOnDisk(Target.Sequence->GetPackage()->GetName(), PackageFile);
    const bool bDeleteReported = UEditorAssetLibrary::DeleteAsset(AssetPath);
    const bool bStillThere = UEditorAssetLibrary::DoesAssetExist(AssetPath);
    const bool bFileStillThere = !PackageFile.IsEmpty() && IFileManager::Get().FileExists(*PackageFile);
    const bool bActuallyDeleted = bDeleteReported && !bStillThere && !bFileStillThere;

    TSharedPtr<FJsonObject> Report = LevelSequenceUtils::NewNamePayload(SequenceName);
    Report->SetStringField(TEXT("asset_path"), AssetPath);
    Report->SetBoolField(TEXT("delete_attempted"), bDeleteReported);
    Report->SetBoolField(TEXT("asset_still_exists"), bStillThere);
    Report->SetBoolField(TEXT("file_still_exists"), bFileStillThere);
    Report->SetBoolField(TEXT("actually_deleted"), bActuallyDeleted);
    return mcp_common::MakeOutcomeReply(Report, bActuallyDeleted, bActuallyDeleted ? FString() : FString::Printf(
        TEXT("delete_level_sequence: '%s' was not deleted: %s."), *AssetPath,
        bFileStillThere ? TEXT("its file is still on disk (read-only, or held open by another process)")
            : bStillThere ? TEXT("the asset is still registered (it may be open in an editor)")
            : TEXT("the delete reported failure")));
}

// --- Playback length and frame rate stored in an asset ------------------------------

// MCP:set_sequence_duration - Sets the playback range of a Level Sequence (found by asset name under /Game) to [0, duration) and marks the package dirty without saving; not undoable. Returns previous_duration, actual_duration, duration_set (within 0.01 s), and start_frame and end_frame in tick-resolution frames.
//   @param sequence_name - Asset name of the Level Sequence, not its path (e.g. "LS_Intro"); the first match under /Game is used
//   @param duration - New length in seconds, greater than 0 and at most 3600 (0 is refused)
//   @returns { sequence_name: string, requested_duration: number, previous_duration: number, actual_duration: number, duration_set: bool, start_frame: int, end_frame: int }
TSharedPtr<FJsonObject> HandleSetSequenceDuration(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentTimelineImpl;

    FString SequenceName;
    double Duration = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("duration"), Duration)) { return Missing; }
    if (SequenceName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("sequence_name cannot be empty"));
    }
    if (Duration <= 0.0 || Duration > 3600.0)
    {
        return mcp_common::MakeErrorReply(TEXT("Duration must be between 0.0 and 3600.0 seconds"));
    }

    const Impl::FResolvedSequence Target = Impl::ResolveSequence(SequenceName, TEXT("Movie scene not found in level sequence"));
    if (Target.Error.IsValid())
    {
        return Target.Error;
    }

    // The range becomes [0, duration), counted in ticks of the scene's own resolution,
    // which itself is left alone.
    const FFrameRate TickRate = Target.MovieScene->GetTickResolution();
    const double PreviousSeconds = Impl::RangeSeconds(Target.MovieScene->GetPlaybackRange(), TickRate);

    const FFrameNumber LastTick(static_cast<int32>(Duration * TickRate.AsDecimal()));
    Target.MovieScene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), LastTick));
    Target.Sequence->MarkPackageDirty();

    const TRange<FFrameNumber> StoredRange = Target.MovieScene->GetPlaybackRange();
    const double StoredSeconds = Impl::RangeSeconds(StoredRange, TickRate);

    TSharedPtr<FJsonObject> Report = LevelSequenceUtils::NewNamePayload(SequenceName);
    Report->SetNumberField(TEXT("requested_duration"), Duration);
    Report->SetNumberField(TEXT("previous_duration"), PreviousSeconds);
    Report->SetNumberField(TEXT("actual_duration"), StoredSeconds);
    // Whole ticks cannot hit every duration exactly; a hundredth of a second is close enough.
    Report->SetBoolField(TEXT("duration_set"), FMath::IsNearlyEqual(StoredSeconds, Duration, 0.01));
    Report->SetNumberField(TEXT("start_frame"), StoredRange.GetLowerBoundValue().Value);
    Report->SetNumberField(TEXT("end_frame"), StoredRange.GetUpperBoundValue().Value);
    return mcp_common::MakeOkReply(Report);
}

// MCP:get_sequence_duration - Reads the playback range of a Level Sequence, the first under /Game whose asset name matches (case-insensitive), without changing it. Returns duration in seconds, and start_frame and end_frame in tick-resolution frames, not display frames; get_timeline_info also gives both rates.
//   @param sequence_name - Asset name of the Level Sequence, not its path (e.g. "LS_Intro"); the first match under /Game is used
//   @returns { sequence_name: string, duration: number, start_frame: int, end_frame: int }
TSharedPtr<FJsonObject> HandleGetSequenceDuration(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentTimelineImpl;

    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const Impl::FResolvedSequence Target = Impl::ResolveSequence(SequenceName, TEXT("Movie scene not found"));
    if (Target.Error.IsValid())
    {
        return Target.Error;
    }

    const TRange<FFrameNumber> Range = Target.MovieScene->GetPlaybackRange();

    TSharedPtr<FJsonObject> Report = LevelSequenceUtils::NewNamePayload(SequenceName);
    Report->SetNumberField(TEXT("duration"), Impl::RangeSeconds(Range, Target.MovieScene->GetTickResolution()));
    Report->SetNumberField(TEXT("start_frame"), Range.GetLowerBoundValue().Value);
    Report->SetNumberField(TEXT("end_frame"), Range.GetUpperBoundValue().Value);
    return mcp_common::MakeOkReply(Report);
}

// MCP:set_sequence_frame_rate - Change the frame rate of a Level Sequence: its tick resolution AND its display rate, as create_level_sequence sets them (one tick per frame). Every key, section and range is migrated to the new rate, so nothing moves in time. To change only the display rate, use set_sequence_display_rate.
//   @param sequence_name - Name of the sequence to modify
//   @param frame_rate - New frame rate in fps, must be greater than 0; NTSC rates (23.976, 29.97, 59.94) become N*1000/1001
//   @returns { sequence_name: string, frame_rate: number, numerator: int, denominator: int, previous_frame_rate: number, frame_rate_set: bool } -- read back from the movie scene
TSharedPtr<FJsonObject> HandleSetSequenceFrameRate(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentTimelineImpl;

    FString SequenceName;
    double FrameRate = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("frame_rate"), FrameRate)) { return Missing; }
    if (FrameRate <= 0.0)
    {
        return mcp_common::MakeErrorReply(TEXT("Frame rate must be greater than 0"));
    }

    const Impl::FResolvedSequence Target = Impl::ResolveSequence(SequenceName, TEXT("Movie scene not found"));
    if (Target.Error.IsValid())
    {
        return Target.Error;
    }
    UMovieScene* Scene = Target.MovieScene;

    // A whole rate is N/1; an NTSC rate (N*1000/1001 fps) keeps its exact ratio; anything else
    // is kept to a thousandth of a frame.
    const double NtscBase = FrameRate * 1.001;
    FFrameRate NewRate(FMath::RoundToInt32(FrameRate), 1);
    if (!FMath::IsNearlyEqual(FrameRate, FMath::RoundToDouble(FrameRate), 1e-6))
    {
        NewRate = FMath::IsNearlyEqual(NtscBase, FMath::RoundToDouble(NtscBase), 1e-3)
            ? FFrameRate(FMath::RoundToInt32(NtscBase) * 1000, 1001)
            : FFrameRate(FMath::RoundToInt32(FrameRate * 1000.0), 1000);
    }

    const FFrameRate OldRate = Scene->GetTickResolution();
    {
        const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetSequenceFrameRate", "MCP: Set Sequence Frame Rate"));
        // Rescales every frame number from the old tick resolution to the new one and then sets it.
        UE::MovieScene::TimeHelpers::MigrateFrameTimes(OldRate, NewRate, Scene);
        Scene->SetDisplayRate(NewRate);
        Target.Sequence->MarkPackageDirty();
    }

    const FFrameRate StoredTick = Scene->GetTickResolution();
    const bool bSet = StoredTick == NewRate && Scene->GetDisplayRate() == NewRate;

    TSharedPtr<FJsonObject> Report = LevelSequenceUtils::NewNamePayload(SequenceName);
    Report->SetNumberField(TEXT("frame_rate"), StoredTick.AsDecimal());
    Report->SetNumberField(TEXT("numerator"), StoredTick.Numerator);
    Report->SetNumberField(TEXT("denominator"), StoredTick.Denominator);
    Report->SetNumberField(TEXT("previous_frame_rate"), OldRate.AsDecimal());
    Report->SetBoolField(TEXT("frame_rate_set"), bSet);
    return mcp_common::MakeOutcomeReply(Report, bSet,
        bSet ? FString() : FString(TEXT("set_sequence_frame_rate: the movie scene does not read back the requested rate.")));
}

// MCP:get_sequence_frame_rate - Reads the tick resolution of a Level Sequence (found by asset name under /Game), NOT its display rate, without changing it. Returns frame_rate as a decimal plus numerator and denominator. Read the display rate with get_timeline_info; change both with set_sequence_frame_rate.
//   @param sequence_name - Asset name of the Level Sequence, not its path (e.g. "LS_Intro"); the first match under /Game is used
//   @returns { sequence_name: string, frame_rate: number, numerator: int, denominator: int }
TSharedPtr<FJsonObject> HandleGetSequenceFrameRate(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentTimelineImpl;

    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const Impl::FResolvedSequence Target = Impl::ResolveSequence(SequenceName, TEXT("Movie scene not found"));
    if (Target.Error.IsValid())
    {
        return Target.Error;
    }

    // The reported rate is the tick resolution, not the display rate.
    const FFrameRate TickRate = Target.MovieScene->GetTickResolution();

    TSharedPtr<FJsonObject> Report = LevelSequenceUtils::NewNamePayload(SequenceName);
    Report->SetNumberField(TEXT("frame_rate"), TickRate.AsDecimal());
    Report->SetNumberField(TEXT("numerator"), TickRate.Numerator);
    Report->SetNumberField(TEXT("denominator"), TickRate.Denominator);
    return mcp_common::MakeOkReply(Report);
}

// ---------------------------------------------------------------------------
// Runtime playback through ULevelSequencePlayer.
//
// A player spawns an ALevelSequenceActor, and Play() needs a game tick context (the
// movie-scene tick manager) that a plain editor world never sets up: playing there
// asserts. The play command therefore insists on a PIE or game world and returns a
// clean error otherwise. Started players are remembered by a player_key so the other
// runtime commands can reach them.
// ---------------------------------------------------------------------------

// MCP:play_level_sequence_runtime - Create a runtime player and start playback of a Level Sequence
//   @param sequence_name - Name of the sequence to play
//   @param world - Optional world selector: absent/'pie' (default: PlayWorld, else the first PIE context) | 'server' | 'client' | 'pie:N' | 'game' (alias of 'pie'). Runtime playback needs a PIE/Game world (an editor world has no movie-scene tick manager and would assert), so an editor world is refused here.
//   @returns { sequence_name: string, player_key: string, playing: bool, current_time: number, world: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }
TSharedPtr<FJsonObject> HandlePlayLevelSequenceRuntime(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentTimelineImpl;

    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (GEditor == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("GEditor is not available"));
    }

    FString WorldSelector;
    UWorld* PlaybackWorld = Impl::ResolveRuntimePlaybackWorld(Params, WorldSelector);
    if (PlaybackWorld == nullptr)
    {
        // Nothing is spawned into the editor world; the caller is told how to get a
        // world that can actually play the sequence.
        return mcp_common::MakeErrorReply(
            TEXT("Runtime level-sequence playback requires a running game/PIE world. ")
            TEXT("Start Play-In-Editor (PIE) first, then call play_level_sequence_runtime. ")
            TEXT("To inspect or scrub a sequence in the editor without PIE, use play_timeline / set_timeline_time / get_timeline_info instead."));
    }

    const Impl::FResolvedSequence Target = Impl::ResolveSequence(SequenceName);
    if (Target.Error.IsValid())
    {
        return Target.Error;
    }

    // Default playback settings; the player comes with the actor that hosts it.
    ALevelSequenceActor* HostActor = nullptr;
    ULevelSequencePlayer* Player = ULevelSequencePlayer::CreateLevelSequencePlayer(
        PlaybackWorld, Target.Sequence, FMovieSceneSequencePlaybackSettings(), HostActor);
    if (Player == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create Level Sequence Player"));
    }
    Player->Play();

    // Sequence name plus the host actor's address: playing the same sequence twice gives
    // two independent keys.
    const FString PlayerKey = FString::Printf(TEXT("%s_%p"), *SequenceName, HostActor);
    RuntimePlayersByKey.Add(PlayerKey, Player);
    RuntimeActorsByKey.Add(PlayerKey, HostActor);

    TSharedPtr<FJsonObject> Report = LevelSequenceUtils::NewNamePayload(SequenceName);
    Report->SetStringField(TEXT("player_key"), PlayerKey);
    Report->SetBoolField(TEXT("playing"), Player->IsPlaying());
    Report->SetNumberField(TEXT("current_time"), Player->GetCurrentTime().AsSeconds());
    // Which world is playing it: a cinematic started in the other PIE instance would
    // otherwise look exactly like one that never started.
    Report->SetStringField(TEXT("world"), WorldSelector);
    mcp_common::AddWorldEvidence(Report, PlaybackWorld);
    return mcp_common::MakeOkReply(Report);
}

// MCP:pause_level_sequence_runtime - Pauses, at its current position, the runtime ULevelSequencePlayer whose player_key play_level_sequence_runtime returned. Players live in the PIE or game world and stop resolving once it ends. Returns player_key, paused, playing and current_time in seconds; read it later with get_level_sequence_status_runtime.
//   @param player_key - Player key returned from play_level_sequence_runtime
//   @returns { player_key: string, paused: bool, playing: bool, current_time: number }
TSharedPtr<FJsonObject> HandlePauseLevelSequenceRuntime(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentTimelineImpl;

    FString PlayerKey;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("player_key"), PlayerKey, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const Impl::FRuntimePlayerLookup Found = Impl::LookUpRuntimePlayer(RuntimePlayersByKey, PlayerKey);
    if (Found.Error.IsValid())
    {
        return Found.Error;
    }
    Found.Player->Pause();

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("player_key"), PlayerKey);
    Report->SetBoolField(TEXT("paused"), Found.Player->IsPaused());
    Report->SetBoolField(TEXT("playing"), Found.Player->IsPlaying());
    Report->SetNumberField(TEXT("current_time"), Found.Player->GetCurrentTime().AsSeconds());
    return mcp_common::MakeOkReply(Report);
}

// MCP:stop_level_sequence_runtime - Stops the runtime ULevelSequencePlayer whose player_key play_level_sequence_runtime returned, forgets the key and destroys the ALevelSequenceActor hosting it, so the key is invalid afterwards. Returns player_key, stopped (true when a playing or paused player was stopped, read back after Stop) and playing as read just after Stop. A player that had already stopped (it reached its end, say) is still released: success with stopped false and a note. A player still playing or paused after Stop is an error, and the key and its actor are kept. Use pause_level_sequence_runtime to keep the player.
//   @param player_key - Player key returned from play_level_sequence_runtime
//   @returns { player_key: string, stopped: bool, playing: bool, note?: string }
TSharedPtr<FJsonObject> HandleStopLevelSequenceRuntime(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentTimelineImpl;

    FString PlayerKey;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("player_key"), PlayerKey, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const Impl::FRuntimePlayerLookup Found = Impl::LookUpRuntimePlayer(RuntimePlayersByKey, PlayerKey);
    if (Found.Error.IsValid())
    {
        return Found.Error;
    }
    // Stop only acts on a playing or paused player, and defers itself while the player is evaluating, so the
    // state is read on both sides of the call. A stop that did not take keeps the key and the actor.
    const bool bWasRunning = Found.Player->IsPlaying() || Found.Player->IsPaused();
    Found.Player->Stop();
    const bool bStillPlaying = Found.Player->IsPlaying();
    if (bWasRunning && (bStillPlaying || Found.Player->IsPaused()))
    {
        TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
        Report->SetStringField(TEXT("player_key"), PlayerKey);
        Report->SetBoolField(TEXT("stopped"), false);
        Report->SetBoolField(TEXT("playing"), bStillPlaying);
        return mcp_common::MakeOutcomeReply(Report, false,
            TEXT("stop_level_sequence_runtime: the player is still playing or paused after Stop (it may be mid-evaluation); the key and its actor are kept, call again"));
    }

    // Forget the player and tear down the actor that hosted it.
    RuntimePlayersByKey.Remove(PlayerKey);
    TWeakObjectPtr<ALevelSequenceActor> HostActor;
    RuntimeActorsByKey.RemoveAndCopyValue(PlayerKey, HostActor);
    if (ALevelSequenceActor* Host = HostActor.Get())
    {
        Host->Destroy();
    }

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("player_key"), PlayerKey);
    Report->SetBoolField(TEXT("stopped"), bWasRunning);
    Report->SetBoolField(TEXT("playing"), bStillPlaying);
    if (!bWasRunning)
    {
        Report->SetStringField(TEXT("note"),
            TEXT("The player was already stopped; its key was released and its actor destroyed."));
    }
    return mcp_common::MakeOkReply(Report);
}

// MCP:set_level_sequence_time_runtime - Set the playback time of an active Level Sequence player
//   @param player_key - Player key returned from play_level_sequence_runtime
//   @param time - Playback time in seconds, must be >= 0
//   @returns { player_key: string, requested_time: number, actual_time: number, frame_rate: number }
TSharedPtr<FJsonObject> HandleSetLevelSequenceTimeRuntime(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentTimelineImpl;

    FString PlayerKey;
    double TimeInSeconds = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("player_key"), PlayerKey, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("time"), TimeInSeconds)) { return Missing; }
    if (TimeInSeconds < 0.0)
    {
        return mcp_common::MakeErrorReply(TEXT("Time must be >= 0.0"));
    }

    const Impl::FRuntimePlayerLookup Found = Impl::LookUpRuntimePlayer(RuntimePlayersByKey, PlayerKey);
    if (Found.Error.IsValid())
    {
        return Found.Error;
    }

    // The float constructor takes seconds. Play moves the position the way normal
    // playback would, rather than jumping.
    Found.Player->SetPlaybackPosition(
        FMovieSceneSequencePlaybackParams(static_cast<float>(TimeInSeconds), EUpdatePositionMethod::Play));

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("player_key"), PlayerKey);
    Report->SetNumberField(TEXT("requested_time"), TimeInSeconds);
    Report->SetNumberField(TEXT("actual_time"), Found.Player->GetCurrentTime().AsSeconds());
    Report->SetNumberField(TEXT("frame_rate"), Found.Player->GetFrameRate().AsDecimal());
    return mcp_common::MakeOkReply(Report);
}

// MCP:get_level_sequence_status_runtime - Retrieve the current playback status of an active Level Sequence player
//   @param player_key - Player key returned from play_level_sequence_runtime
//   @returns { player_key: string, status: string, is_playing: bool, is_paused: bool, is_reversed: bool, current_time: number, duration: number, frame_rate: number, playback_progress: number }
TSharedPtr<FJsonObject> HandleGetLevelSequenceStatusRuntime(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentTimelineImpl;

    FString PlayerKey;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("player_key"), PlayerKey, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const Impl::FRuntimePlayerLookup Found = Impl::LookUpRuntimePlayer(RuntimePlayersByKey, PlayerKey);
    if (Found.Error.IsValid())
    {
        return Found.Error;
    }

    const ULevelSequencePlayer& Player = *Found.Player;
    const bool bPlaying = Player.IsPlaying();
    const bool bPaused = Player.IsPaused();
    const bool bReversed = Player.IsReversed();
    const double NowSeconds = Player.GetCurrentTime().AsSeconds();
    const double LengthSeconds = Player.GetDuration().AsSeconds();
    const double FramesPerSecond = Player.GetFrameRate().AsDecimal();

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("player_key"), PlayerKey);
    Report->SetStringField(TEXT("status"), Impl::PlaybackStatusName(bPaused, bPlaying));
    Report->SetBoolField(TEXT("is_playing"), bPlaying);
    Report->SetBoolField(TEXT("is_paused"), bPaused);
    Report->SetBoolField(TEXT("is_reversed"), bReversed);
    Report->SetNumberField(TEXT("current_time"), NowSeconds);
    Report->SetNumberField(TEXT("duration"), LengthSeconds);
    Report->SetNumberField(TEXT("frame_rate"), FramesPerSecond);
    // Percentage through the sequence; 0 for a zero-length sequence.
    Report->SetNumberField(TEXT("playback_progress"), LengthSeconds > 0.0 ? (NowSeconds / LengthSeconds) * 100.0 : 0.0);
    return mcp_common::MakeOkReply(Report);
}

// ---------------------------------------------------------------------------
// Editor-side timeline introspection / Sequencer editor control.
//
// get_timeline_info / get_sequence_tracks read the ULevelSequence's UMovieScene
// directly (works headless). play_timeline / pause_timeline / set_timeline_time
// drive the open Sequencer editor via ULevelSequenceEditorBlueprintLibrary; if no
// Sequencer can be opened (e.g. fully headless), they return an honest structured
// result describing why, never a fake success.
// ---------------------------------------------------------------------------

// MCP:get_timeline_info - Reads the timeline summary of a Level Sequence asset (first asset-name match under /Game); needs no open Sequencer and changes nothing. Returns asset_path, start_frame and end_frame in ticks, duration in seconds, tick_resolution, display_rate with numerator and denominator, and track, binding, possessable and spawnable counts. master_track_count counts the root tracks, the camera cut track included, as get_sequence_tracks lists them.
//   @param sequence_name - Asset name of the Level Sequence, not its path (e.g. "LS_Intro"); the first match under /Game is used
//   @returns { sequence_name: string, asset_path: string, start_frame: int, end_frame: int, duration: number, tick_resolution: number, display_rate: number, display_rate_numerator: int, display_rate_denominator: int, master_track_count: int, binding_count: int, possessable_count: int, spawnable_count: int }
TSharedPtr<FJsonObject> HandleGetTimelineInfo(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentTimelineImpl;

    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = Impl::ReadTimelineTarget(Params, SequenceName)) { return Missing; }

    const Impl::FResolvedSequence Target = Impl::ResolveSequence(SequenceName, TEXT("Movie scene not found"));
    if (Target.Error.IsValid())
    {
        return Target.Error;
    }

    // Read through a const pointer so the const (non-deprecated) GetBindings() is the one used.
    const UMovieScene* Scene = Target.MovieScene;
    const TRange<FFrameNumber> Range = Scene->GetPlaybackRange();
    const FFrameRate TickResolution = Scene->GetTickResolution();
    const FFrameRate DisplayRate = Scene->GetDisplayRate();
    const FFrameNumber StartFrame = Range.GetLowerBoundValue();
    const FFrameNumber EndFrame = Range.GetUpperBoundValue();
    const double TicksPerSecond = TickResolution.AsDecimal();

    // Root tracks counted as get_sequence_tracks lists them: the tracks of GetTracks() plus the camera cut
    // track, which the movie scene keeps apart from GetTracks().
    int32 RootTrackCount = (Scene->GetCameraCutTrack() != nullptr) ? 1 : 0;
    for (const UMovieSceneTrack* Track : Scene->GetTracks())
    {
        if (Track != nullptr)
        {
            ++RootTrackCount;
        }
    }

    TSharedPtr<FJsonObject> Report = LevelSequenceUtils::NewNamePayload(SequenceName);
    Report->SetStringField(TEXT("asset_path"), Target.Sequence->GetPathName());
    Report->SetNumberField(TEXT("start_frame"), StartFrame.Value);
    Report->SetNumberField(TEXT("end_frame"), EndFrame.Value);
    Report->SetNumberField(TEXT("duration"), TicksPerSecond > 0.0 ? (EndFrame - StartFrame).Value / TicksPerSecond : 0.0);
    Report->SetNumberField(TEXT("tick_resolution"), TicksPerSecond);
    Report->SetNumberField(TEXT("display_rate"), DisplayRate.AsDecimal());
    Report->SetNumberField(TEXT("display_rate_numerator"), DisplayRate.Numerator);
    Report->SetNumberField(TEXT("display_rate_denominator"), DisplayRate.Denominator);
    Report->SetNumberField(TEXT("master_track_count"), RootTrackCount);
    Report->SetNumberField(TEXT("binding_count"), Scene->GetBindings().Num());
    Report->SetNumberField(TEXT("possessable_count"), Scene->GetPossessableCount());
    Report->SetNumberField(TEXT("spawnable_count"), Scene->GetSpawnableCount());
    return mcp_common::MakeOkReply(Report);
}

// MCP:get_sequence_tracks - Lists the tracks of a Level Sequence, the first asset under /Game whose name matches sequence_name (case-insensitive): root (master) tracks, the camera cut track included, with display name, track name, class and section count, then each object binding (guid, display name) with its own tracks nested. Read-only; needs no open Sequencer. total_track_count is the number of tracks listed: root tracks plus every binding's tracks. For flat lists carrying binding_guid use get_sequence_master_tracks; for spawnable/possessable flags use get_sequence_bindings; for one binding use get_binding_tracks.
//   @param sequence_name - Name of the target level sequence
//   @returns { sequence_name: string, master_tracks: [object], bindings: [object], master_track_count: int, binding_count: int, total_track_count: int }
TSharedPtr<FJsonObject> HandleGetSequenceTracks(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentTimelineImpl;

    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = Impl::ReadTimelineTarget(Params, SequenceName)) { return Missing; }

    const Impl::FResolvedSequence Target = Impl::ResolveSequence(SequenceName, TEXT("Movie scene not found"));
    if (Target.Error.IsValid())
    {
        return Target.Error;
    }

    // Const view, again for the non-deprecated GetBindings().
    const UMovieScene* Scene = Target.MovieScene;

    // Root tracks, not bound to any object.
    TArray<TSharedPtr<FJsonValue>> RootTracks;
    for (const UMovieSceneTrack* Track : Scene->GetTracks())
    {
        if (Track != nullptr)
        {
            RootTracks.Add(MakeShared<FJsonValueObject>(Impl::DescribeTrack(*Track, /*bIncludeTrackName*/ true)));
        }
    }
    // The camera cut track is a root track too, but the movie scene keeps it apart from GetTracks().
    if (const UMovieSceneTrack* CameraCutTrack = Scene->GetCameraCutTrack())
    {
        RootTracks.Add(MakeShared<FJsonValueObject>(Impl::DescribeTrack(*CameraCutTrack, /*bIncludeTrackName*/ true)));
    }

    // Object bindings (possessables and spawnables), each with its own tracks.
    TArray<TSharedPtr<FJsonValue>> Bindings;
    int32 BoundTrackCount = 0;
    for (const FMovieSceneBinding& Binding : Scene->GetBindings())
    {
        const FGuid BindingId = Binding.GetObjectGuid();

        TArray<TSharedPtr<FJsonValue>> BoundTracks;
        for (const UMovieSceneTrack* Track : Binding.GetTracks())
        {
            if (Track != nullptr)
            {
                BoundTracks.Add(MakeShared<FJsonValueObject>(Impl::DescribeTrack(*Track, /*bIncludeTrackName*/ false)));
            }
        }

        BoundTrackCount += BoundTracks.Num();

        TSharedPtr<FJsonObject> BindingEntry = MakeShared<FJsonObject>();
        BindingEntry->SetStringField(TEXT("guid"), BindingId.ToString());
        BindingEntry->SetStringField(TEXT("display_name"), Scene->GetObjectDisplayName(BindingId).ToString());
        BindingEntry->SetArrayField(TEXT("tracks"), BoundTracks);
        Bindings.Add(MakeShared<FJsonValueObject>(BindingEntry));
    }

    TSharedPtr<FJsonObject> Report = LevelSequenceUtils::NewNamePayload(SequenceName);
    Report->SetArrayField(TEXT("master_tracks"), RootTracks);
    Report->SetArrayField(TEXT("bindings"), Bindings);
    Report->SetNumberField(TEXT("master_track_count"), RootTracks.Num());
    Report->SetNumberField(TEXT("binding_count"), Bindings.Num());
    // Every track listed above: the root tracks plus the tracks of every binding.
    Report->SetNumberField(TEXT("total_track_count"), RootTracks.Num() + BoundTrackCount);
    return mcp_common::MakeOkReply(Report);
}

// MCP:set_timeline_time - Set the playhead time of the open Sequencer editor on a target sequence
//   @param sequence_name - Name of the target level sequence
//   @param time - Playback time in seconds, must be >= 0
//   @returns { sequence_name: string, requested_time: number, actual_frame: int, time_set: bool }
TSharedPtr<FJsonObject> HandleSetTimelineTime(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentTimelineImpl;

    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = Impl::ReadTimelineTarget(Params, SequenceName)) { return Missing; }
    double TimeInSeconds = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("time"), TimeInSeconds)) { return Missing; }
    if (TimeInSeconds < 0.0)
    {
        return mcp_common::MakeErrorReply(TEXT("time must be >= 0.0"));
    }

    if (TSharedPtr<FJsonObject> Failure = Impl::FocusSequencerOn(SequenceName)) { return Failure; }

    // Jump the playhead (no playback in between) to a position given in seconds, then read
    // back where it landed, in display-rate frames.
    ULevelSequenceEditorBlueprintLibrary::SetGlobalPosition(
        FMovieSceneSequencePlaybackParams(static_cast<float>(TimeInSeconds), EUpdatePositionMethod::Jump),
        EMovieSceneTimeUnit::DisplayRate);
    const FMovieSceneSequencePlaybackParams Landed =
        ULevelSequenceEditorBlueprintLibrary::GetGlobalPosition(EMovieSceneTimeUnit::DisplayRate);

    TSharedPtr<FJsonObject> Report = LevelSequenceUtils::NewNamePayload(SequenceName);
    Report->SetNumberField(TEXT("requested_time"), TimeInSeconds);
    Report->SetNumberField(TEXT("actual_frame"), Landed.Frame.FrameNumber.Value);
    Report->SetBoolField(TEXT("time_set"), true);
    return mcp_common::MakeOkReply(Report);
}

// MCP:play_timeline - Starts playback in the editor's Sequencer on a Level Sequence found by asset name under /Game, first opening it there when it is not the sequence shown. Editor preview only, no PIE or game player (use play_level_sequence_runtime for that). Returns at once and changes no asset; calling it while playing keeps playing. playing is read right after the call. Errors when the sequence is missing or no Sequencer can show it. Pause with pause_timeline; move the playhead with set_timeline_time.
//   @param sequence_name - Name of the target level sequence
//   @returns { sequence_name: string, playing: bool }
//   @hints additive idempotent
TSharedPtr<FJsonObject> HandlePlayTimeline(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentTimelineImpl;

    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = Impl::ReadTimelineTarget(Params, SequenceName)) { return Missing; }

    if (TSharedPtr<FJsonObject> Failure = Impl::FocusSequencerOn(SequenceName)) { return Failure; }

    ULevelSequenceEditorBlueprintLibrary::Play();

    TSharedPtr<FJsonObject> Report = LevelSequenceUtils::NewNamePayload(SequenceName);
    Report->SetBoolField(TEXT("playing"), ULevelSequenceEditorBlueprintLibrary::IsPlaying());
    return mcp_common::MakeOkReply(Report);
}

// MCP:pause_timeline - Pause playback in the open Sequencer editor on a target sequence
//   @param sequence_name - Name of the target level sequence
//   @returns { sequence_name: string, playing: bool, paused: bool }
TSharedPtr<FJsonObject> HandlePauseTimeline(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentTimelineImpl;

    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = Impl::ReadTimelineTarget(Params, SequenceName)) { return Missing; }

    if (TSharedPtr<FJsonObject> Failure = Impl::FocusSequencerOn(SequenceName)) { return Failure; }

    ULevelSequenceEditorBlueprintLibrary::Pause();

    // paused is simply the negation of the Sequencer's playing flag after the call.
    const bool bStillPlaying = ULevelSequenceEditorBlueprintLibrary::IsPlaying();
    TSharedPtr<FJsonObject> Report = LevelSequenceUtils::NewNamePayload(SequenceName);
    Report->SetBoolField(TEXT("playing"), bStillPlaying);
    Report->SetBoolField(TEXT("paused"), !bStillPlaying);
    return mcp_common::MakeOkReply(Report);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("create_level_sequence"), &HandleCreateLevelSequence },
    { TEXT("delete_level_sequence"), &HandleDeleteLevelSequence },
    { TEXT("get_level_sequence_status_runtime"), &HandleGetLevelSequenceStatusRuntime },
    { TEXT("get_level_sequences"), &HandleGetLevelSequences },
    { TEXT("get_sequence_duration"), &HandleGetSequenceDuration },
    { TEXT("get_sequence_frame_rate"), &HandleGetSequenceFrameRate },
    { TEXT("get_sequence_tracks"), &HandleGetSequenceTracks },
    { TEXT("get_timeline_info"), &HandleGetTimelineInfo },
    { TEXT("pause_level_sequence_runtime"), &HandlePauseLevelSequenceRuntime },
    { TEXT("pause_timeline"), &HandlePauseTimeline },
    { TEXT("play_level_sequence_runtime"), &HandlePlayLevelSequenceRuntime },
    { TEXT("play_timeline"), &HandlePlayTimeline },
    { TEXT("set_level_sequence_time_runtime"), &HandleSetLevelSequenceTimeRuntime },
    { TEXT("set_sequence_duration"), &HandleSetSequenceDuration },
    { TEXT("set_sequence_frame_rate"), &HandleSetSequenceFrameRate },
    { TEXT("set_timeline_time"), &HandleSetTimelineTime },
    { TEXT("stop_level_sequence_runtime"), &HandleStopLevelSequenceRuntime },
};
} // namespace UEAgentTimelineCommands
