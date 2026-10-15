#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Media/LevelSequenceUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

// Level sequence / movie scene asset model
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneObjectBindingID.h"

// Tracks / sections (MovieSceneTracks module)
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Tracks/MovieScene3DAttachTrack.h"           // UMovieScene3DAttachTrack::AddConstraint
#include "Sections/MovieScene3DAttachSection.h"       // UMovieScene3DAttachSection (GetConstraintBindingID/socket/component)
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Tracks/MovieSceneSpawnTrack.h" // MovieScene module: detect spawnable bindings

// Extra tracks (MovieSceneTracks module)
#include "Tracks/MovieSceneAudioTrack.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Tracks/MovieSceneFadeTrack.h"
#include "Sections/MovieSceneFadeSection.h"

// Marked frames (MovieScene module)
#include "MovieSceneMarkedFrame.h"

// Sound asset for audio sections (Engine module)
#include "Sound/SoundBase.h"

// Channels (MovieScene module)
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneBoolChannel.h"

// Property / skeletal-animation tracks (MovieSceneTracks module)
#include "Tracks/MovieScenePropertyTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieSceneVectorTrack.h"     // declares UMovieSceneFloatVectorTrack
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneVisibilityTrack.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"
#include "Tracks/MovieSceneSlomoTrack.h"                  // time-dilation master track
#include "Tracks/MovieSceneLevelVisibilityTrack.h"        // level-visibility master track
#include "Sections/MovieSceneLevelVisibilitySection.h"
#include "Sections/MovieSceneVectorSection.h" // declares UMovieSceneFloatVectorSection

// Skeletal animation asset (Engine module)
#include "Animation/AnimSequenceBase.h"

// High-res screenshot capture: triggered via GEditor->Exec("HighResShot ...")
// (handled in UnrealEdSrv.cpp). Output path resolved via FPaths.
#include "Misc/Paths.h"

// Cinematic camera (CinematicCamera module)
#include "CineCameraActor.h"

// Common runtime actor classes used for spawnables
#include "Engine/StaticMeshActor.h"
#include "Engine/PointLight.h"

// Subsequence / shot tracks + component binding
#include "Tracks/MovieSceneSubTrack.h"            // MovieScene module
#include "Sections/MovieSceneSubSection.h"        // MovieScene module
#include "Tracks/MovieSceneCinematicShotTrack.h"  // MovieSceneTracks module
#include "Sections/MovieSceneCinematicShotSection.h"
#include "Components/ActorComponent.h"

// Sub-section authoring (event/cameracut/audio section types + camera cut track)
#include "Sections/MovieSceneEventSection.h"              // MovieSceneTracks module
#include "Sections/MovieSceneAudioSection.h"              // MovieSceneTracks module

// Section easing / completion mode / blend type
#include "MovieSceneSection.h"
#include "Evaluation/MovieSceneCompletionMode.h"
#include "Evaluation/Blending/MovieSceneBlendType.h"

// Spawnables / possessables (advanced)
#include "MovieSceneSpawnable.h"
// Spawn ownership of custom spawnable bindings
#include "MovieSceneBindingReferences.h"                  // FMovieSceneBindingReferences::GetCustomBinding
#include "Bindings/MovieSceneSpawnableBinding.h"          // UMovieSceneSpawnableBindingBase + ESpawnOwnership field
#include "Components/StaticMeshComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/PointLightComponent.h"
#include "UObject/UObjectGlobals.h"  // DuplicateObject

// Material parameter tracks (MovieSceneTracks) + MPC asset (Engine) + curve tangents (MovieScene)
#include "Tracks/MovieSceneMaterialTrack.h"                       // UMovieSceneComponentMaterialTrack, FComponentMaterialInfo, EComponentMaterialType
#include "Tracks/MovieSceneMaterialParameterCollectionTrack.h"    // UMovieSceneMaterialParameterCollectionTrack
#include "Materials/MaterialParameterCollection.h"                // UMaterialParameterCollection
#include "Curves/RichCurve.h"                                     // ERichCurveInterpMode / ERichCurveTangentMode

// Section-property setters
#include "Sections/MovieSceneSkeletalAnimationSection.h"          // UMovieSceneSkeletalAnimationSection + FMovieSceneSkeletalAnimationParams (MovieSceneTracks)
#include "Evaluation/MovieSceneSectionParameters.h"               // FMovieSceneSectionParameters (MovieScene)
#include "Variants/MovieSceneTimeWarpVariant.h"                   // FMovieSceneTimeWarpVariant + EMovieSceneTimeWarpType (MovieScene)
#include "Sound/SoundAttenuation.h"                               // USoundAttenuation (Engine)

// Sequencer folder organization (UMovieScene root/child folders, MovieScene module)
#include "MovieSceneFolder.h"                                     // UMovieSceneFolder (AddChildTrack/AddChildFolder/AddChildObjectBinding) + UMovieScene Add/Get/RemoveRootFolder

#define LOCTEXT_NAMESPACE "UEAgentSequencer"

/**
 * Sequencer authoring commands: the cinematic authoring chain on the Level Sequences that
 * UEAgentTimelineCommands creates -- bindings (spawnable / possessable) -> tracks -> sections -> keyframes
 * -> camera cuts.
 *
 * Every command works directly on the ULevelSequence / UMovieScene asset model, so it is fully headless (no
 * open Sequencer editor required); changes persist by saving the owning package. The low-level MovieScene /
 * MovieSceneTracks / CinematicCamera runtime APIs are used instead of the SequencerScripting plugin, so no
 * extra plugin dependency is needed.
 */
namespace UEAgentSequencerCommands
{

// MCP-DOMAIN: Timeline

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace UEAgentSequencerImpl
{

    // The sequence's movie scene, or nullptr with OutError naming what is missing (RequestedAs when the sequence is).
    UMovieScene* MovieSceneOf(ULevelSequence* Sequence, const FString& RequestedAs, FString& OutError)
    {
        if (Sequence == nullptr)
        {
            OutError = TEXT("Level sequence '") + RequestedAs + TEXT("' not found");
            return nullptr;
        }

        UMovieScene* MovieScene = Sequence->GetMovieScene();
        if (MovieScene == nullptr)
        {
            OutError = TEXT("Movie scene not found");
        }
        return MovieScene;
    }

    // The movie scene of Sequence (looked up as RequestedAs) into OutMovieScene; the error reply when the sequence or
    // its movie scene is missing.
    template <typename MovieSceneType>
    TSharedPtr<FJsonObject> OpenMovieScene(ULevelSequence* Sequence, const FString& RequestedAs, MovieSceneType*& OutMovieScene)
    {
        FString Error;
        OutMovieScene = MovieSceneOf(Sequence, RequestedAs, Error);
        if (OutMovieScene == nullptr)
        {
            return mcp_common::MakeErrorReply(Error);
        }
        return nullptr;
    }

    // Reads sequence_path or its alias sequence_name; the missing-parameter reply when neither has a value.
    TSharedPtr<FJsonObject> ReadSequencePath(const TSharedPtr<FJsonObject>& Params, FString& OutSequencePath)
    {
        if (!Params->TryGetStringField(TEXT("sequence_path"), OutSequencePath) || OutSequencePath.IsEmpty())
        {
            Params->TryGetStringField(TEXT("sequence_name"), OutSequencePath);
        }
        if (OutSequencePath.IsEmpty())
        {
            return mcp_common::MissingParameterReply(TEXT("sequence_path"));
        }
        return nullptr;
    }

    // ReadSequencePath with both names tested in one condition, the form the tool catalog publishes as two
    // required params.
    TSharedPtr<FJsonObject> ReadSequencePathOrName(const TSharedPtr<FJsonObject>& Params, FString& OutSequencePath)
    {
        if ((!Params->TryGetStringField(TEXT("sequence_path"), OutSequencePath) || OutSequencePath.IsEmpty()) &&
            (!Params->TryGetStringField(TEXT("sequence_name"), OutSequencePath) || OutSequencePath.IsEmpty()))
        {
            return mcp_common::MissingParameterReply(TEXT("sequence_path"));
        }
        return nullptr;
    }

    // GuidStr parsed into OutGuid; the "<ParamName> is not a valid GUID" reply when it is not a valid GUID.
    TSharedPtr<FJsonObject> ParseGuid(const FString& GuidStr, const TCHAR* ParamName, FGuid& OutGuid)
    {
        if (!FGuid::Parse(GuidStr, OutGuid) || !OutGuid.IsValid())
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s is not a valid GUID"), ParamName));
        }
        return nullptr;
    }

    // Section as SectionType into OutTyped; otherwise the "Resolved section is <class>, not a <TypeName>" reply.
    template <typename SectionType>
    TSharedPtr<FJsonObject> CastSection(UMovieSceneSection* Section, const TCHAR* TypeName, SectionType*& OutTyped)
    {
        OutTyped = Cast<SectionType>(Section);
        if (OutTyped == nullptr)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Resolved section is %s, not a %s"), *Section->GetClass()->GetName(), TypeName));
        }
        return nullptr;
    }

    // A binding ID for Guid relative to the sequence that holds it (the form SetCameraBindingID uses).
    FMovieSceneObjectBindingID RelativeBindingID(const FGuid& Guid)
    {
        return FMovieSceneObjectBindingID{ UE::MovieScene::FRelativeObjectBindingID(Guid) };
    }

    // Reads the required sequence_name and binding_guid; returns the reply for the first one missing.
    TSharedPtr<FJsonObject> ReadSequenceAndBinding(const TSharedPtr<FJsonObject>& Params, FString& OutSequenceName, FString& OutGuidStr)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), OutSequenceName)) { return Missing; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("binding_guid"), OutGuidStr)) { return Missing; }
        return nullptr;
    }

    // Whether Guid is a possessable, a spawnable or any other binding of the movie scene.
    bool IsKnownBinding(UMovieScene* MovieScene, const FGuid& Guid)
    {
        if (MovieScene->FindPossessable(Guid) || MovieScene->FindSpawnable(Guid))
        {
            return true;
        }
        return const_cast<const UMovieScene*>(MovieScene)->GetBindings().ContainsByPredicate(
            [&Guid](const FMovieSceneBinding& B){ return B.GetObjectGuid() == Guid; });
    }

    // Finds ActorName in the editor world; returns the error reply when the world or the actor is missing.
    TSharedPtr<FJsonObject> FindActorInEditorWorld(const FString& ActorName, UWorld*& OutWorld, AActor*& OutActor)
    {
        if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(OutWorld))
        {
            return NoWorld;
        }

        OutActor = mcp_common::FindActorByName(OutWorld, ActorName);
        if (OutActor == nullptr)
        {
            return mcp_common::MakeErrorReply(TEXT("Actor '") + ActorName + TEXT("' not found in the editor world"));
        }
        return nullptr;
    }

    // The binding's TrackType track, added when missing; bOutCreated reports the addition.
    template <typename TrackType>
    TrackType* FindOrAddTrack(UMovieScene* MovieScene, const FGuid& BindingGuid, bool& bOutCreated)
    {
        bOutCreated = false;
        TrackType* Track = MovieScene->FindTrack<TrackType>(BindingGuid);
        if (!Track)
        {
            Track = MovieScene->AddTrack<TrackType>(BindingGuid);
            bOutCreated = (Track != nullptr);
        }
        return Track;
    }

    // The root TrackType track, added when missing; bOutCreated reports the addition.
    template <typename TrackType>
    TrackType* FindOrAddTrack(UMovieScene* MovieScene, bool& bOutCreated)
    {
        bOutCreated = false;
        TrackType* Track = MovieScene->FindTrack<TrackType>();
        if (!Track)
        {
            Track = MovieScene->AddTrack<TrackType>();
            bOutCreated = (Track != nullptr);
        }
        return Track;
    }

    // The track's first section as SectionType, else a new one spanning the playback range (bOutCreated).
    template <typename SectionType, typename TrackType>
    SectionType* FirstOrNewPlaybackSection(UMovieScene* MovieScene, TrackType* Track, bool& bOutCreated)
    {
        bOutCreated = false;
        SectionType* Section = (Track->GetAllSections().Num() > 0) ? Cast<SectionType>(Track->GetAllSections()[0]) : nullptr;
        if (!Section)
        {
            Section = Cast<SectionType>(Track->CreateNewSection());
            if (Section)
            {
                Section->SetRange(MovieScene->GetPlaybackRange());
                Track->AddSection(*Section);
                bOutCreated = true;
            }
        }
        return Section;
    }

    // Keys Location, Rotation (roll, pitch, yaw) and Scale at Frame on a transform section's 9 double channels;
    // the number of channels keyed, 0 when the section has fewer than 9.
    int32 KeyTransformChannels(TArrayView<FMovieSceneDoubleChannel*> Channels, FFrameNumber Frame,
        const FVector& Location, const FRotator& Rotation, const FVector& Scale)
    {
        if (Channels.Num() < 9)
        {
            return 0;
        }
        const double Values[9] = {
            Location.X, Location.Y, Location.Z,
            Rotation.Roll, Rotation.Pitch, Rotation.Yaw,
            Scale.X, Scale.Y, Scale.Z
        };
        int32 KeysAdded = 0;
        for (int32 i = 0; i < 9; ++i)
        {
            if (FMovieSceneDoubleChannel* Channel = Channels[i])
            {
                Channel->GetData().UpdateOrAddKey(Frame, FMovieSceneDoubleValue(Values[i]));
                ++KeysAdded;
            }
        }
        return KeysAdded;
    }

    // Widens the section's closed bounds so that KeyFrame falls inside its range.
    void ExtendSectionToFrame(UMovieSceneSection* Section, FFrameNumber KeyFrame)
    {
        const TRange<FFrameNumber> Range = Section->GetRange();
        const bool bBelow = Range.GetLowerBound().IsClosed() && KeyFrame < Range.GetLowerBoundValue();
        const bool bAbove = Range.GetUpperBound().IsClosed() && KeyFrame > Range.GetUpperBoundValue();
        if (bBelow) { Section->SetStartFrame(TRangeBound<FFrameNumber>::Inclusive(KeyFrame)); }
        if (bAbove) { Section->SetEndFrame(TRangeBound<FFrameNumber>::Exclusive(KeyFrame + 1)); }
    }
}

// Resolve a friendly actor class name to a UClass. Handles short names (no
// ANY_PACKAGE, so FindObject(nullptr,"Foo") does not find them) and
// "/Script/Module.Class" style paths.
UClass* ResolveActorClass(const FString& ClassName)
{
    if (ClassName.IsEmpty())
    {
        return nullptr;
    }

    // Common friendly aliases mapped to verified script paths / classes.
    const FString Lower = ClassName.ToLower();
    if (Lower == TEXT("cinecameraactor") || Lower == TEXT("cinecamera") || Lower == TEXT("cine_camera"))
    {
        return ACineCameraActor::StaticClass();
    }
    if (Lower == TEXT("staticmeshactor") || Lower == TEXT("staticmesh"))
    {
        return AStaticMeshActor::StaticClass();
    }
    if (Lower == TEXT("pointlight"))
    {
        return APointLight::StaticClass();
    }

    // Full object/script path.
    if (ClassName.Contains(TEXT(".")) || ClassName.StartsWith(TEXT("/Script/")))
    {
        if (UClass* Loaded = LoadObject<UClass>(nullptr, *ClassName))
        {
            return Loaded;
        }
    }

    // Short name lookup.
    if (UClass* Found = UClass::TryFindTypeSlow<UClass>(ClassName))
    {
        return Found;
    }
    if (UClass* First = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::None))
    {
        return First;
    }
    return nullptr;
}

bool SaveSequencePackage(ULevelSequence* Sequence)
{
    if (!Sequence)
    {
        return false;
    }
    Sequence->MarkPackageDirty();

    UPackage* Package = Sequence->GetOutermost();
    if (!Package)
    {
        return false;
    }
    return mcp_common::SaveAssetPackage(Package, Sequence);
}

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------

// MCP:add_spawnable_to_sequence - Adds a spawnable actor binding to a Level Sequence (asset-model, headless).
//   @param sequence_name - Name of the target Level Sequence (found by AssetName under /Game).
//   @param class_name - AActor subclass name (e.g. "CineCameraActor" default, "StaticMeshActor", "PointLight", or a "/Script/Module.Class" path).
//   @returns { sequence_name: string, binding_guid: string, display_name: string, class_name: string, spawnable: bool, saved: bool }

TSharedPtr<FJsonObject> HandleAddSpawnableToSequence(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }
    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    FString ClassName = TEXT("CineCameraActor");
    Params->TryGetStringField(TEXT("class_name"), ClassName);

    UClass* SpawnClass = ResolveActorClass(ClassName);
    if (!SpawnClass || !SpawnClass->IsChildOf(AActor::StaticClass()))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Could not resolve actor class '%s' (must be an AActor subclass)"), *ClassName));
    }

    const FScopedTransaction Transaction(LOCTEXT("AddSpawnable", "MCP: Add Spawnable To Sequence"));
    MovieScene->Modify();

    // ULevelSequence::CreateSpawnable accepts a UClass* (verified: the spawnable
    // actor binding's SupportsBindingCreationFromObject accepts an AActor subclass)
    // and works headlessly on the asset model (no Sequencer UI required).
    // The override is protected on ULevelSequence but public on the UMovieSceneSequence
    // base, so call through a base pointer (virtual dispatch still hits the override).
    const FGuid NewGuid = static_cast<UMovieSceneSequence*>(Sequence)->CreateSpawnable(SpawnClass);
    if (!NewGuid.IsValid())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to create spawnable binding for class '%s'"), *ClassName));
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("binding_guid"), NewGuid.ToString());
    Response->SetStringField(TEXT("display_name"), MovieScene->GetObjectDisplayName(NewGuid).ToString());
    Response->SetStringField(TEXT("class_name"), SpawnClass->GetName());
    Response->SetBoolField(TEXT("spawnable"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_spawnable_to_sequence"));
}

// MCP:bind_actor_to_sequence - Adds a possessable binding to a Level Sequence for an existing editor-world actor.
//   @param sequence_name - Name of the target Level Sequence.
//   @param actor_name - Label (or name) of the actor in the current editor world to possess.
//   @returns { sequence_name: string, actor_name: string, binding_guid: string, display_name: string, possessable: bool, saved: bool }

TSharedPtr<FJsonObject> HandleBindActorToSequence(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    UWorld* World = nullptr;
    AActor* TargetActor = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::FindActorInEditorWorld(ActorName, World, TargetActor))
    {
        return Failure;
    }

    const FScopedTransaction Transaction(LOCTEXT("BindActor", "MCP: Bind Actor To Sequence"));
    MovieScene->Modify();

    // CreatePossessable both adds the possessable and binds the object (uses the
    // actor's world as playback context). Verified ULevelSequence::FindOrAddBinding.
    // Protected override on ULevelSequence, public on the UMovieSceneSequence base.
    const FGuid NewGuid = static_cast<UMovieSceneSequence*>(Sequence)->CreatePossessable(TargetActor);
    if (!NewGuid.IsValid())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to create possessable binding for actor '%s'"), *ActorName));
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("actor_name"), TargetActor->GetActorLabel());
    Response->SetStringField(TEXT("binding_guid"), NewGuid.ToString());
    Response->SetStringField(TEXT("display_name"), MovieScene->GetObjectDisplayName(NewGuid).ToString());
    Response->SetBoolField(TEXT("possessable"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("bind_actor_to_sequence"));
}

// MCP:get_sequence_bindings - Lists every object binding of a Level Sequence, the first asset under /Game whose name matches sequence_name (case-insensitive), with each binding's tracks (class and section count); read-only, needs no open Sequencer. spawnable is true when the binding has a spawn track and possessable otherwise; possessable_count and spawnable_count are the movie scene's own counts and can disagree with those flags. Root tracks are not listed: use get_sequence_tracks. For one binding's tracks with display names use get_binding_tracks.
//   @param sequence_name - Name of the target Level Sequence.
//   @returns { sequence_name: string, bindings: [{guid, name, spawnable, possessable, track_count, tracks: [{class, section_count}]}], binding_count: int, possessable_count: int, spawnable_count: int }

TSharedPtr<FJsonObject> HandleGetSequenceBindings(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }
    // A const movie scene, for the non-deprecated GetBindings()/GetObjectDisplayName() const overloads.
    const UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(LevelSequenceUtils::FindByName(SequenceName), SequenceName, MovieScene)) { return Failure; }

    TArray<TSharedPtr<FJsonValue>> BindingsArray;
    for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
    {
        const FGuid Guid = Binding.GetObjectGuid();

        TSharedPtr<FJsonObject> BindingObj = MakeShared<FJsonObject>();
        BindingObj->SetStringField(TEXT("guid"), Guid.ToString());
        BindingObj->SetStringField(TEXT("name"), MovieScene->GetObjectDisplayName(Guid).ToString());

        // A binding is spawnable iff a spawn track exists for its guid (LevelSequence
        // spawnables are custom bindings recorded as possessables + a spawn track).
        const bool bSpawnable = MovieScene->FindTrack<UMovieSceneSpawnTrack>(Guid) != nullptr;
        BindingObj->SetBoolField(TEXT("spawnable"), bSpawnable);
        BindingObj->SetBoolField(TEXT("possessable"), !bSpawnable);
        BindingObj->SetNumberField(TEXT("track_count"), Binding.GetTracks().Num());

        // Track summaries.
        TArray<TSharedPtr<FJsonValue>> TrackArr;
        for (const UMovieSceneTrack* Track : Binding.GetTracks())
        {
            if (!Track) { continue; }
            TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
            TrackObj->SetStringField(TEXT("class"), Track->GetClass()->GetName());
            TrackObj->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
            TrackArr.Add(MakeShared<FJsonValueObject>(TrackObj));
        }
        BindingObj->SetArrayField(TEXT("tracks"), TrackArr);

        BindingsArray.Add(MakeShared<FJsonValueObject>(BindingObj));
    }

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetArrayField(TEXT("bindings"), BindingsArray);
    Response->SetNumberField(TEXT("binding_count"), BindingsArray.Num());
    Response->SetNumberField(TEXT("possessable_count"), MovieScene->GetPossessableCount());
    Response->SetNumberField(TEXT("spawnable_count"), MovieScene->GetSpawnableCount());
    return mcp_common::MakeOkReply(Response);
}

// ---------------------------------------------------------------------------
// Tracks / sections / keyframes
// ---------------------------------------------------------------------------

// MCP:add_transform_track_to_binding - Adds a 3D transform track (with a section spanning the playback range) to a binding.
//   @param sequence_name - Name of the target Level Sequence.
//   @param binding_guid - GUID of the binding (from add_spawnable_to_sequence / bind_actor_to_sequence / get_sequence_bindings).
//   @returns { sequence_name: string, binding_guid: string, track_class: string, track_added: bool, section_created: bool, section_count: int, saved: bool }

TSharedPtr<FJsonObject> HandleAddTransformTrackToBinding(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName, GuidStr;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequenceAndBinding(Params, SequenceName, GuidStr)) { return Missing; }

    FGuid BindingGuid;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentSequencerImpl::ParseGuid(GuidStr, TEXT("binding_guid"), BindingGuid)) { return Invalid; }

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    if (!UEAgentSequencerImpl::IsKnownBinding(MovieScene, BindingGuid))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Binding '%s' not found in sequence"), *GuidStr));
    }

    const FScopedTransaction Transaction(LOCTEXT("AddTransformTrack", "MCP: Add Transform Track"));
    MovieScene->Modify();

    bool bCreatedTrack = false;
    UMovieScene3DTransformTrack* Track = UEAgentSequencerImpl::FindOrAddTrack<UMovieScene3DTransformTrack>(MovieScene, BindingGuid, bCreatedTrack);
    if (!Track)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to add 3D transform track"));
    }

    // Ensure the track has a section spanning the playback range so keys evaluate.
    bool bCreatedSection = false;
    UMovieSceneSection* Section = UEAgentSequencerImpl::FirstOrNewPlaybackSection<UMovieSceneSection>(MovieScene, Track, bCreatedSection);
    if (!Section)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create transform section"));
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("binding_guid"), GuidStr);
    Response->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
    Response->SetBoolField(TEXT("track_added"), bCreatedTrack);
    Response->SetBoolField(TEXT("section_created"), bCreatedSection);
    Response->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_transform_track_to_binding"));
}

// MCP:add_attach_track - Add a Sequencer 3D-Attach track on a binding so it follows another binding (e.g. a prop attached to a character's hand socket) over the section's range.  The attachee binding owns a UMovieScene3DAttachTrack; AddConstraint creates an attach section pointing at the target binding.  Idempotent by (target_binding_guid, start_frame).
//   @param sequence_name - Name of the target Level Sequence.
//   @param binding_guid - GUID of the binding to attach (the attachee; this binding owns the attach track).
//   @param target_binding_guid - GUID of the binding to attach TO (the parent/target).
//   @param start_frame - Optional start frame of the attach section (default: the sequence playback range start).
//   @param duration - Optional duration in frames (default: the playback range size).
//   @param socket_name - Optional socket name on the target to attach to (default: none -> target root).
//   @param component_name - Optional component name on the target to attach to (default: none).
//   @returns { sequence_name: string, binding_guid: string, target_binding_guid: string, track_added: bool, already_exists: bool, start_frame: int, duration: int, socket_name: string, component_name: string, section_count: int, track_class: string, saved: bool }
TSharedPtr<FJsonObject> HandleAddAttachTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName, GuidStr, TargetGuidStr;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequenceAndBinding(Params, SequenceName, GuidStr)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target_binding_guid"), TargetGuidStr)) { return Missing; }

    FGuid BindingGuid, TargetGuid;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentSequencerImpl::ParseGuid(GuidStr, TEXT("binding_guid"), BindingGuid)) { return Invalid; }
    if (TSharedPtr<FJsonObject> Invalid = UEAgentSequencerImpl::ParseGuid(TargetGuidStr, TEXT("target_binding_guid"), TargetGuid)) { return Invalid; }

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    // Both the attachee and the target must be real bindings in this sequence.
    if (!UEAgentSequencerImpl::IsKnownBinding(MovieScene, BindingGuid))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("binding_guid '%s' not found in sequence"), *GuidStr));
    }
    if (!UEAgentSequencerImpl::IsKnownBinding(MovieScene, TargetGuid))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("target_binding_guid '%s' not found in sequence"), *TargetGuidStr));
    }

    // Frame range: default to the sequence playback range; override with start_frame/duration.
    const TRange<FFrameNumber> Playback = MovieScene->GetPlaybackRange();
    const int32 DefaultStart = Playback.GetLowerBound().IsClosed() ? Playback.GetLowerBoundValue().Value : 0;
    const int32 DefaultEnd = Playback.GetUpperBound().IsClosed() ? Playback.GetUpperBoundValue().Value : (DefaultStart + 1000);
    int32 StartFrame = DefaultStart;
    double StartFrameD = 0.0;
    if (Params->TryGetNumberField(TEXT("start_frame"), StartFrameD)) { StartFrame = FMath::RoundToInt(StartFrameD); }
    int32 Duration = FMath::Max(1, DefaultEnd - DefaultStart);
    double DurationD = 0.0;
    if (Params->TryGetNumberField(TEXT("duration"), DurationD)) { Duration = FMath::Max(1, FMath::RoundToInt(DurationD)); }

    FString SocketName, ComponentName;
    Params->TryGetStringField(TEXT("socket_name"), SocketName);
    Params->TryGetStringField(TEXT("component_name"), ComponentName);

    const FScopedTransaction Transaction(LOCTEXT("AddAttachTrack", "MCP: Add Attach Track"));
    MovieScene->Modify();

    bool bCreatedTrack = false;
    UMovieScene3DAttachTrack* Track = UEAgentSequencerImpl::FindOrAddTrack<UMovieScene3DAttachTrack>(MovieScene, BindingGuid, bCreatedTrack);
    if (!Track)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to add 3D attach track"));
    }

    // Idempotency: skip if a section already attaches to the same target at the same start frame.
    bool bAlreadyExists = false;
    for (UMovieSceneSection* Sec : Track->GetAllSections())
    {
        UMovieScene3DAttachSection* AttachSec = Cast<UMovieScene3DAttachSection>(Sec);
        if (!AttachSec) { continue; }
        const TRange<FFrameNumber> R = AttachSec->GetRange();
        const int32 SecStart = R.GetLowerBound().IsClosed() ? R.GetLowerBoundValue().Value : DefaultStart;
        if (AttachSec->GetConstraintBindingID().GetGuid() == TargetGuid && SecStart == StartFrame)
        {
            bAlreadyExists = true;
            break;
        }
    }

    if (!bAlreadyExists)
    {
        UMovieSceneSection* NewSection = Track->AddConstraint(
            FFrameNumber(StartFrame), Duration, FName(*SocketName), FName(*ComponentName), UEAgentSequencerImpl::RelativeBindingID(TargetGuid));
        if (!NewSection)
        {
            return mcp_common::MakeErrorReply(TEXT("Failed to add attach constraint section"));
        }
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("binding_guid"), GuidStr);
    Response->SetStringField(TEXT("target_binding_guid"), TargetGuidStr);
    Response->SetBoolField(TEXT("track_added"), bCreatedTrack);
    Response->SetBoolField(TEXT("already_exists"), bAlreadyExists);
    Response->SetNumberField(TEXT("start_frame"), StartFrame);
    Response->SetNumberField(TEXT("duration"), Duration);
    Response->SetStringField(TEXT("socket_name"), SocketName);
    Response->SetStringField(TEXT("component_name"), ComponentName);
    Response->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
    Response->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_attach_track"));
}

// ---------------------------------------------------------------------------
// Set the 6 attach/detach rules on a 3D-attach section (add_attach_track
// creates the section with all 6 rules at defaults). All 6 are public UPROPERTY
// EAttachmentRule/EDetachmentRule on UMovieScene3DAttachSection -> direct write + Modify().
// ---------------------------------------------------------------------------
static FString AttachmentRuleToString(EAttachmentRule R)
{
    const UEnum* E = StaticEnum<EAttachmentRule>();
    return E ? E->GetNameStringByValue((int64)R) : FString();
}
static FString DetachmentRuleToString(EDetachmentRule R)
{
    const UEnum* E = StaticEnum<EDetachmentRule>();
    return E ? E->GetNameStringByValue((int64)R) : FString();
}
static bool ParseAttachmentRule(const FString& S, EAttachmentRule& Out)
{
    const UEnum* E = StaticEnum<EAttachmentRule>();
    if (!E) { return false; }
    int64 V = E->GetValueByNameString(S);
    if (V == INDEX_NONE) { V = E->GetValueByNameString(FString(TEXT("EAttachmentRule::")) + S); }
    if (V == INDEX_NONE) { return false; }
    Out = (EAttachmentRule)V;
    return true;
}
static bool ParseDetachmentRule(const FString& S, EDetachmentRule& Out)
{
    const UEnum* E = StaticEnum<EDetachmentRule>();
    if (!E) { return false; }
    int64 V = E->GetValueByNameString(S);
    if (V == INDEX_NONE) { V = E->GetValueByNameString(FString(TEXT("EDetachmentRule::")) + S); }
    if (V == INDEX_NONE) { return false; }
    Out = (EDetachmentRule)V;
    return true;
}
// Shared reader: the 6 rules of a 3D-attach section as a JSON object.
static TSharedPtr<FJsonObject> AttachRulesJson(const UMovieScene3DAttachSection* S)
{
    TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
    J->SetStringField(TEXT("attachment_location_rule"), AttachmentRuleToString(S->AttachmentLocationRule));
    J->SetStringField(TEXT("attachment_rotation_rule"), AttachmentRuleToString(S->AttachmentRotationRule));
    J->SetStringField(TEXT("attachment_scale_rule"), AttachmentRuleToString(S->AttachmentScaleRule));
    J->SetStringField(TEXT("detachment_location_rule"), DetachmentRuleToString(S->DetachmentLocationRule));
    J->SetStringField(TEXT("detachment_rotation_rule"), DetachmentRuleToString(S->DetachmentRotationRule));
    J->SetStringField(TEXT("detachment_scale_rule"), DetachmentRuleToString(S->DetachmentScaleRule));
    return J;
}

// Defined with the section helpers further down, after the ones it builds on.
TSharedPtr<FJsonObject> ResolveSectionFromParams(const TSharedPtr<FJsonObject>& Params, ULevelSequence*& OutSequence, UMovieSceneSection*& OutSection);

// MCP:set_attach_section_rules - Set the attach/detach transform rules on a Sequencer 3D-attach section (created by add_attach_track).  The 6 rules (location/rotation/scale x attach/detach) control how the attachee's transform is preserved when the attach begins/ends. Only the supplied rules change (applied_count) and all 6 are read back. The section is resolved like get_section_info (binding_guid + track_index + section_index).
//   @param sequence_name - Name of the target Level Sequence (or sequence_path).
//   @param sequence_path - Asset path of the target Level Sequence (alias for sequence_name).
//   @param binding_guid - GUID of the attachee binding that owns the attach track.
//   @param binding_name_or_guid - Alias for binding_guid, read only when binding_guid is absent; it must also be a GUID string, a bound object's name is not resolved.
//   @param track_index - Optional index into the binding's tracks (default 0).
//   @param track_type - Optional class filter to pick the track ("transform"|"float"|"bool"|"vector"|"color"|"visibility"|"audio"|"event"|"fade"|"skeletalanimation"|"subsequence"|"cameracut"|"property"); used when track_index is omitted.
//   @param section_index - Optional index of the section on the track (default 0).
//   @param attachment_location_rule - Optional EAttachmentRule: KeepRelative | KeepWorld | SnapToTarget.
//   @param attachment_rotation_rule - Optional EAttachmentRule: KeepRelative | KeepWorld | SnapToTarget.
//   @param attachment_scale_rule - Optional EAttachmentRule: KeepRelative | KeepWorld | SnapToTarget.
//   @param detachment_location_rule - Optional EDetachmentRule: KeepRelative | KeepWorld.
//   @param detachment_rotation_rule - Optional EDetachmentRule: KeepRelative | KeepWorld.
//   @param detachment_scale_rule - Optional EDetachmentRule: KeepRelative | KeepWorld.
//   @returns { sequence_path: string, attach_rules: object, applied_count: int, applied: bool }
TSharedPtr<FJsonObject> HandleSetAttachSectionRules(const TSharedPtr<FJsonObject>& Params)
{
    ULevelSequence* Sequence = nullptr;
    UMovieSceneSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ResolveSectionFromParams(Params, Sequence, Section)) { return Failure; }
    UMovieScene3DAttachSection* AttachSec = Cast<UMovieScene3DAttachSection>(Section);
    if (!AttachSec)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Resolved section is not a 3D-attach section (create one with add_attach_track; check track_index/section_index)"));
    }

    const FScopedTransaction Transaction(LOCTEXT("SetAttachSectionRules", "MCP: Set Attach Section Rules"));
    AttachSec->Modify();

    int32 AppliedCount = 0;
    FString S;
    EAttachmentRule AR;
    EDetachmentRule DR;

    if (Params->TryGetStringField(TEXT("attachment_location_rule"), S))
    {
        if (!ParseAttachmentRule(S, AR)) { return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid attachment_location_rule '%s' (KeepRelative|KeepWorld|SnapToTarget)"), *S)); }
        AttachSec->AttachmentLocationRule = AR; ++AppliedCount;
    }
    if (Params->TryGetStringField(TEXT("attachment_rotation_rule"), S))
    {
        if (!ParseAttachmentRule(S, AR)) { return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid attachment_rotation_rule '%s' (KeepRelative|KeepWorld|SnapToTarget)"), *S)); }
        AttachSec->AttachmentRotationRule = AR; ++AppliedCount;
    }
    if (Params->TryGetStringField(TEXT("attachment_scale_rule"), S))
    {
        if (!ParseAttachmentRule(S, AR)) { return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid attachment_scale_rule '%s' (KeepRelative|KeepWorld|SnapToTarget)"), *S)); }
        AttachSec->AttachmentScaleRule = AR; ++AppliedCount;
    }
    if (Params->TryGetStringField(TEXT("detachment_location_rule"), S))
    {
        if (!ParseDetachmentRule(S, DR)) { return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid detachment_location_rule '%s' (KeepRelative|KeepWorld)"), *S)); }
        AttachSec->DetachmentLocationRule = DR; ++AppliedCount;
    }
    if (Params->TryGetStringField(TEXT("detachment_rotation_rule"), S))
    {
        if (!ParseDetachmentRule(S, DR)) { return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid detachment_rotation_rule '%s' (KeepRelative|KeepWorld)"), *S)); }
        AttachSec->DetachmentRotationRule = DR; ++AppliedCount;
    }
    if (Params->TryGetStringField(TEXT("detachment_scale_rule"), S))
    {
        if (!ParseDetachmentRule(S, DR)) { return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid detachment_scale_rule '%s' (KeepRelative|KeepWorld)"), *S)); }
        AttachSec->DetachmentScaleRule = DR; ++AppliedCount;
    }

    if (AppliedCount == 0)
        return mcp_common::MakeErrorReply(
            TEXT("No rules provided. Supply at least one of: attachment_location_rule, attachment_rotation_rule, attachment_scale_rule, detachment_location_rule, detachment_rotation_rule, detachment_scale_rule"));

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetObjectField(TEXT("attach_rules"), AttachRulesJson(AttachSec));
    Response->SetNumberField(TEXT("applied_count"), AppliedCount);
    Response->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_attach_section_rules"));
}

// MCP:add_transform_keyframe - Adds one transform keyframe (9 double channels) at a time on a binding's transform section.  Requires a transform track to already exist (call add_transform_track_to_binding first).
//   @param sequence_name - Name of the target Level Sequence.
//   @param binding_guid - GUID of the binding.
//   @param time_seconds - Time of the key in seconds (>= 0). Converted to ticks via tick resolution.
//   @param location - Optional [x, y, z] world location.
//   @param rotation - Optional [pitch, yaw, roll] world rotation.
//   @param scale - Optional [x, y, z] scale (default 1,1,1).
//   @returns { sequence_name: string, binding_guid: string, time_seconds: float, key_frame: int, keys_added: int, channel_count: int, saved: bool }

TSharedPtr<FJsonObject> HandleAddTransformKeyframe(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName, GuidStr;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequenceAndBinding(Params, SequenceName, GuidStr)) { return Missing; }

    double TimeSeconds = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("time_seconds"), TimeSeconds)) { return Missing; }
    if (TimeSeconds < 0.0)
    {
        return mcp_common::MakeErrorReply(TEXT("time_seconds must be >= 0.0"));
    }

    FGuid BindingGuid;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentSequencerImpl::ParseGuid(GuidStr, TEXT("binding_guid"), BindingGuid)) { return Invalid; }

    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));
    const FRotator Rotation = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));
    FVector Scale(1.0, 1.0, 1.0);
    if (Params->HasField(TEXT("scale")))
    {
        Scale = mcp_common::ReadJsonVector(Params, TEXT("scale"));
    }

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    if (!MovieScene->FindBinding(BindingGuid))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("binding_guid '%s' not found in sequence"), *GuidStr));
    }
    UMovieScene3DTransformTrack* Track = MovieScene->FindTrack<UMovieScene3DTransformTrack>(BindingGuid);
    if (!Track)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No 3D transform track on this binding. Call add_transform_track_to_binding first."));
    }

    UMovieScene3DTransformSection* Section = nullptr;
    if (Track->GetAllSections().Num() > 0)
    {
        Section = Cast<UMovieScene3DTransformSection>(Track->GetAllSections()[0]);
    }
    if (!Section)
    {
        return mcp_common::MakeErrorReply(TEXT("Transform track has no transform section"));
    }

    const FScopedTransaction Transaction(LOCTEXT("AddTransformKey", "MCP: Add Transform Keyframe"));
    Section->Modify();

    // Convert seconds -> tick frame number.
    const FFrameRate TickResolution = MovieScene->GetTickResolution();
    const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();

    // Make sure the section range covers this key (otherwise the key is outside the
    // evaluated section). Expand the lower/upper bound if needed.
    UEAgentSequencerImpl::ExtendSectionToFrame(Section, KeyFrame);

    // Transform section channels: index 0-2 Location XYZ, 3-5 Rotation XYZ, 6-8 Scale XYZ
    // (all FMovieSceneDoubleChannel; verified in MovieScene3DTransformSection::CacheChannelProxy).
    TArrayView<FMovieSceneDoubleChannel*> DoubleChannels =
        Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
    const int32 KeysAdded = UEAgentSequencerImpl::KeyTransformChannels(DoubleChannels, KeyFrame, Location, Rotation, Scale);
    if (KeysAdded == 0)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("No transform channels found on section (expected 9, got %d)"), DoubleChannels.Num()));
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("binding_guid"), GuidStr);
    Response->SetNumberField(TEXT("time_seconds"), TimeSeconds);
    Response->SetNumberField(TEXT("key_frame"), KeyFrame.Value);
    Response->SetNumberField(TEXT("keys_added"), KeysAdded);
    Response->SetNumberField(TEXT("channel_count"), DoubleChannels.Num());
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_transform_keyframe"));
}

// MCP:set_section_range - Sets the frame range of the FIRST section of one track on a Level Sequence binding (sequence found by asset name under /Game), in an undo transaction, and saves the sequence. track_index picks the track in the binding's track order; no other section can be targeted, and an unknown binding reads as zero tracks. Frames are tick-resolution frames, rounded; keys are not moved or rescaled. range_set is false when the stored range differs from the request. Read a section's range with get_section_info; for the whole sequence use set_sequence_playback_range.
//   @param sequence_name - Name of the target Level Sequence.
//   @param binding_guid - GUID of the binding.
//   @param start_frame - Inclusive start frame (tick resolution).
//   @param end_frame - Exclusive end frame (must be > start_frame).
//   @param track_index - Index into the binding's tracks (0 = first track, default 0).
//   @returns { sequence_name: string, binding_guid: string, track_index: int, track_class: string, requested_start_frame: int, requested_end_frame: int, applied_start_frame: int, applied_end_frame: int, range_set: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetSectionRange(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName, GuidStr;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequenceAndBinding(Params, SequenceName, GuidStr)) { return Missing; }

    int32 TrackIndex = 0;
    Params->TryGetNumberField(TEXT("track_index"), TrackIndex);

    double StartFrameD = 0.0, EndFrameD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("start_frame"), StartFrameD)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("end_frame"), EndFrameD)) { return Missing; }

    FGuid BindingGuid;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentSequencerImpl::ParseGuid(GuidStr, TEXT("binding_guid"), BindingGuid)) { return Invalid; }

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    const TArray<UMovieSceneTrack*> Tracks = MovieScene->FindTracks(UMovieSceneTrack::StaticClass(), BindingGuid);
    if (!Tracks.IsValidIndex(TrackIndex))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("track_index %d out of range (%d tracks on binding)"), TrackIndex, Tracks.Num()));
    }
    UMovieSceneTrack* Track = Tracks[TrackIndex];
    if (!Track || Track->GetAllSections().Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Selected track has no sections"));
    }

    const int32 StartFrame = FMath::RoundToInt(StartFrameD);
    const int32 EndFrame = FMath::RoundToInt(EndFrameD);
    if (EndFrame <= StartFrame)
    {
        return mcp_common::MakeErrorReply(TEXT("end_frame must be greater than start_frame"));
    }

    UMovieSceneSection* Section = Track->GetAllSections()[0];

    const FScopedTransaction Transaction(LOCTEXT("SetSectionRange", "MCP: Set Section Range"));
    Section->Modify();
    Section->SetRange(TRange<FFrameNumber>(FFrameNumber(StartFrame), FFrameNumber(EndFrame)));

    // Read back the applied range.
    const TRange<FFrameNumber> Applied = Section->GetRange();
    const int32 AppliedStart = Applied.GetLowerBound().IsClosed() ? Applied.GetLowerBoundValue().Value : StartFrame;
    const int32 AppliedEnd = Applied.GetUpperBound().IsClosed() ? Applied.GetUpperBoundValue().Value : EndFrame;

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("binding_guid"), GuidStr);
    Response->SetNumberField(TEXT("track_index"), TrackIndex);
    Response->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
    Response->SetNumberField(TEXT("requested_start_frame"), StartFrame);
    Response->SetNumberField(TEXT("requested_end_frame"), EndFrame);
    Response->SetNumberField(TEXT("applied_start_frame"), AppliedStart);
    Response->SetNumberField(TEXT("applied_end_frame"), AppliedEnd);
    Response->SetBoolField(TEXT("range_set"), (AppliedStart == StartFrame && AppliedEnd == EndFrame));
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_section_range"));
}

// ---------------------------------------------------------------------------
// Camera cuts
// ---------------------------------------------------------------------------

// MCP:add_camera_cut_track - Adds (or returns the existing) camera cut track on a Level Sequence's movie scene.
//   @param sequence_name - Name of the target Level Sequence.
//   @returns { sequence_name: string, track_class: string, track_added: bool, section_count: int, saved: bool }

TSharedPtr<FJsonObject> HandleAddCameraCutTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }
    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    const FScopedTransaction Transaction(LOCTEXT("AddCameraCutTrack", "MCP: Add Camera Cut Track"));
    MovieScene->Modify();

    bool bCreated = false;
    UMovieSceneCameraCutTrack* Track = Cast<UMovieSceneCameraCutTrack>(MovieScene->GetCameraCutTrack());
    if (!Track)
    {
        Track = Cast<UMovieSceneCameraCutTrack>(MovieScene->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass()));
        bCreated = (Track != nullptr);
    }
    if (!Track)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to add camera cut track"));
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
    Response->SetBoolField(TEXT("track_added"), bCreated);
    Response->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_camera_cut_track"));
}

// MCP:add_camera_cut - Adds a camera cut section bound to a camera binding over a frame range.  Creates the camera cut track if it does not yet exist.
//   @param sequence_name - Name of the target Level Sequence.
//   @param camera_binding_guid - GUID of the camera binding the cut switches to.
//   @param start_frame - Inclusive start frame (tick resolution).
//   @param end_frame - Exclusive end frame (must be > start_frame).
//   @returns { sequence_name: string, camera_binding_guid: string, start_frame: int, end_frame: int, section_class: string, section_count: int, saved: bool }

TSharedPtr<FJsonObject> HandleAddCameraCut(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName, GuidStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("camera_binding_guid"), GuidStr)) { return Missing; }

    double StartFrameD = 0.0, EndFrameD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("start_frame"), StartFrameD)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("end_frame"), EndFrameD)) { return Missing; }

    FGuid CameraGuid;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentSequencerImpl::ParseGuid(GuidStr, TEXT("camera_binding_guid"), CameraGuid)) { return Invalid; }

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    const int32 StartFrame = FMath::RoundToInt(StartFrameD);
    const int32 EndFrame = FMath::RoundToInt(EndFrameD);
    if (EndFrame <= StartFrame)
    {
        return mcp_common::MakeErrorReply(TEXT("end_frame must be greater than start_frame"));
    }

    const FScopedTransaction Transaction(LOCTEXT("AddCameraCut", "MCP: Add Camera Cut"));
    MovieScene->Modify();

    UMovieSceneCameraCutTrack* Track = Cast<UMovieSceneCameraCutTrack>(MovieScene->GetCameraCutTrack());
    if (!Track)
    {
        Track = Cast<UMovieSceneCameraCutTrack>(MovieScene->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass()));
    }
    if (!Track)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to obtain camera cut track"));
    }

    UMovieSceneCameraCutSection* Section = Track->AddNewCameraCut(UEAgentSequencerImpl::RelativeBindingID(CameraGuid), FFrameNumber(StartFrame));
    if (!Section)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to add camera cut section"));
    }
    // Apply the explicit requested range.
    Section->Modify();
    Section->SetRange(TRange<FFrameNumber>(FFrameNumber(StartFrame), FFrameNumber(EndFrame)));

    const TRange<FFrameNumber> Applied = Section->GetRange();
    const int32 AppliedStart = Applied.GetLowerBound().IsClosed() ? Applied.GetLowerBoundValue().Value : StartFrame;
    const int32 AppliedEnd = Applied.GetUpperBound().IsClosed() ? Applied.GetUpperBoundValue().Value : EndFrame;

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("camera_binding_guid"), GuidStr);
    Response->SetNumberField(TEXT("start_frame"), AppliedStart);
    Response->SetNumberField(TEXT("end_frame"), AppliedEnd);
    Response->SetStringField(TEXT("section_class"), Section->GetClass()->GetName());
    Response->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_camera_cut"));
}

// MCP:create_cine_camera_in_sequence - Creates a spawnable CineCameraActor binding, keys its transform, and binds a camera cut to it.  High-value one-shot: spawnable CineCameraActor + transform track keyed at frame 0 + a camera cut spanning the playback range.
//   @param sequence_name - Name of the target Level Sequence.
//   @param location - Optional [x, y, z] world location for the camera's key at frame 0.
//   @param rotation - Optional [pitch, yaw, roll] world rotation for the camera's key at frame 0.
//   @returns { sequence_name: string, binding_guid: string, display_name: string, class_name: string, spawnable: bool, transform_keyed: bool, camera_cut_added: bool, saved: bool }

TSharedPtr<FJsonObject> HandleCreateCineCameraInSequence(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }
    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    const FScopedTransaction Transaction(LOCTEXT("CreateCineCamera", "MCP: Create Cine Camera In Sequence"));
    MovieScene->Modify();

    // 1. Spawnable CineCameraActor binding (headless asset-model spawnable).
    //    Protected override on ULevelSequence, public on the UMovieSceneSequence base.
    const FGuid CameraGuid = static_cast<UMovieSceneSequence*>(Sequence)->CreateSpawnable(ACineCameraActor::StaticClass());
    if (!CameraGuid.IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create CineCameraActor spawnable binding"));
    }

    // 2. Add a transform track + section, and key the requested transform at frame 0
    //    (so the spawned camera has a defined starting transform).
    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));
    const FRotator Rotation = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));

    bool bTransformKeyed = false;
    UMovieScene3DTransformTrack* TransformTrack = MovieScene->AddTrack<UMovieScene3DTransformTrack>(CameraGuid);
    if (TransformTrack)
    {
        UMovieScene3DTransformSection* Section = Cast<UMovieScene3DTransformSection>(TransformTrack->CreateNewSection());
        if (Section)
        {
            Section->SetRange(MovieScene->GetPlaybackRange());
            TransformTrack->AddSection(*Section);

            TArrayView<FMovieSceneDoubleChannel*> DoubleChannels =
                Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
            UEAgentSequencerImpl::KeyTransformChannels(DoubleChannels, FFrameNumber(0), Location, Rotation, FVector(1.0, 1.0, 1.0));
            bTransformKeyed = DoubleChannels.Num() >= 9;
        }
    }

    // 3. Add a camera cut track + a cut bound to the camera spanning the playback range.
    UMovieSceneCameraCutTrack* CutTrack = Cast<UMovieSceneCameraCutTrack>(MovieScene->GetCameraCutTrack());
    if (!CutTrack)
    {
        CutTrack = Cast<UMovieSceneCameraCutTrack>(MovieScene->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass()));
    }
    bool bCameraCutAdded = false;
    if (CutTrack)
    {
        const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
        const FFrameNumber Start = PlaybackRange.GetLowerBound().IsClosed() ? PlaybackRange.GetLowerBoundValue() : FFrameNumber(0);
        if (UMovieSceneCameraCutSection* CutSection = CutTrack->AddNewCameraCut(UEAgentSequencerImpl::RelativeBindingID(CameraGuid), Start))
        {
            CutSection->Modify();
            CutSection->SetRange(PlaybackRange);
            bCameraCutAdded = true;
        }
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("binding_guid"), CameraGuid.ToString());
    Response->SetStringField(TEXT("display_name"), MovieScene->GetObjectDisplayName(CameraGuid).ToString());
    Response->SetStringField(TEXT("class_name"), ACineCameraActor::StaticClass()->GetName());
    Response->SetBoolField(TEXT("spawnable"), true);
    Response->SetBoolField(TEXT("transform_keyed"), bTransformKeyed);
    Response->SetBoolField(TEXT("camera_cut_added"), bCameraCutAdded);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("create_cine_camera_in_sequence"));
}

// ---------------------------------------------------------------------------
// Playback range / marked frames
// ---------------------------------------------------------------------------

// MCP:set_sequence_playback_range - Sets the playback range of a Level Sequence found by asset name under /Game, in tick-resolution frames (start inclusive, end exclusive, rounded), in an undo transaction, and saves the sequence. Tracks, sections and keys are not moved or clipped, and the Sequencer's playback-range lock does not block it. range_set compares the stored range with the request. To set a length in seconds from 0 use set_sequence_duration; the editor-only timeline windows are set_sequence_view_range and set_sequence_working_range.
//   @param sequence_name - Name of the target Level Sequence (found by AssetName under /Game).
//   @param start_frame - Inclusive lower bound of the playback range, in tick frames.
//   @param end_frame - Exclusive upper bound of the playback range (must be > start_frame).
//   @returns { sequence_name: string, requested_start_frame: int, requested_end_frame: int, start_frame: int, end_frame: int, range_set: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetSequencePlaybackRange(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }

    double StartFrameD = 0.0, EndFrameD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("start_frame"), StartFrameD)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("end_frame"), EndFrameD)) { return Missing; }

    const int32 StartFrame = FMath::RoundToInt(StartFrameD);
    const int32 EndFrame = FMath::RoundToInt(EndFrameD);
    if (EndFrame <= StartFrame)
    {
        return mcp_common::MakeErrorReply(TEXT("end_frame must be greater than start_frame"));
    }

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    const FScopedTransaction Transaction(LOCTEXT("SetPlaybackRange", "MCP: Set Sequence Playback Range"));
    MovieScene->Modify();

    // SetPlaybackRange(TRange<FFrameNumber>) -- verified MOVIESCENE_API on UMovieScene.
    MovieScene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(StartFrame), FFrameNumber(EndFrame)));

    // Read back the applied range.
    const TRange<FFrameNumber> Applied = MovieScene->GetPlaybackRange();
    const int32 AppliedStart = Applied.GetLowerBound().IsClosed() ? Applied.GetLowerBoundValue().Value : StartFrame;
    const int32 AppliedEnd = Applied.GetUpperBound().IsClosed() ? Applied.GetUpperBoundValue().Value : EndFrame;

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetNumberField(TEXT("requested_start_frame"), StartFrame);
    Response->SetNumberField(TEXT("requested_end_frame"), EndFrame);
    Response->SetNumberField(TEXT("start_frame"), AppliedStart);
    Response->SetNumberField(TEXT("end_frame"), AppliedEnd);
    Response->SetBoolField(TEXT("range_set"), (AppliedStart == StartFrame && AppliedEnd == EndFrame));
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_sequence_playback_range"));
}

// MCP:add_marked_frame - Adds a marked frame (a labelled marker on the Sequencer timeline) at a tick-resolution frame of a Level Sequence found by asset name under /Game, in an undo transaction, and saves the sequence; is_determinism_fence turns the same mark into an evaluation fence. Idempotent per frame: an existing mark at that frame is updated instead of duplicated, its label replaced when one is given and both flags overwritten with the values passed (false when omitted); already_existed says which. Read marks with get_marked_frames, delete one with remove_marked_frame.
//   @param sequence_name - Name of the target Level Sequence.
//   @param frame - Tick frame number to mark.
//   @param label - Optional human-readable label (auto-generated if empty).
//   @param is_determinism_fence - Optional bool (default false): treat the mark as an evaluation fence (cannot be crossed in one evaluation).
//   @param is_inclusive - Optional bool (default false): when a determinism fence, how the previous/next ranges are dissected ((X,Y]->(Y,Z] vs (X,Y)->[Y,Z]).
//   @returns { sequence_name: string, frame: int, mark_index: int, label: string, is_determinism_fence: bool, is_inclusive: bool, already_existed: bool, marked_frame_count: int, saved: bool }

TSharedPtr<FJsonObject> HandleAddMarkedFrame(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }

    double FrameD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("frame"), FrameD)) { return Missing; }
    const int32 Frame = FMath::RoundToInt(FrameD);

    FString Label;
    Params->TryGetStringField(TEXT("label"), Label);

    bool bIsDeterminismFence = false;
    Params->TryGetBoolField(TEXT("is_determinism_fence"), bIsDeterminismFence);
    bool bIsInclusive = false;
    Params->TryGetBoolField(TEXT("is_inclusive"), bIsInclusive);

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    const FScopedTransaction Transaction(LOCTEXT("AddMarkedFrame", "MCP: Add Marked Frame"));
    MovieScene->Modify();

    // Idempotent add-fence: a marked frame is keyed by its frame number, so
    // adding at a frame that already carries a mark must NOT create a duplicate. If one
    // exists, update its fields in place; otherwise add a new mark.
    const int32 ExistingIndex = MovieScene->FindMarkedFrameByFrameNumber(FFrameNumber(Frame));
    const bool bAlreadyExisted = MovieScene->GetMarkedFrames().IsValidIndex(ExistingIndex);

    int32 MarkIndex = ExistingIndex;
    if (bAlreadyExisted)
    {
        // Copy the existing mark (GetMarkedFrames is const-ref), update its auxiliary
        // fields, then delete + re-add at the same frame: AddMarkedFrame appends + sorts,
        // so re-adding the same frame keeps the count and returns the (possibly new) index.
        FMovieSceneMarkedFrame Updated = MovieScene->GetMarkedFrames()[ExistingIndex];
        if (!Label.IsEmpty())
        {
            Updated.Label = Label;
        }
        Updated.bIsDeterminismFence = bIsDeterminismFence;
        Updated.bIsInclusiveTime = bIsInclusive;
        MovieScene->DeleteMarkedFrame(ExistingIndex);
        MarkIndex = MovieScene->AddMarkedFrame(Updated);
    }
    else
    {
        // FMovieSceneMarkedFrame(FFrameNumber) -- verified ctor; Label is a public member.
        // Brace-init to avoid the most-vexing-parse (FFrameNumber(Frame) as a param decl).
        FMovieSceneMarkedFrame MarkedFrame{ FFrameNumber(Frame) };
        if (!Label.IsEmpty())
        {
            MarkedFrame.Label = Label;
        }
        MarkedFrame.bIsDeterminismFence = bIsDeterminismFence;
        MarkedFrame.bIsInclusiveTime = bIsInclusive;
        // AddMarkedFrame returns the index of the newly added mark (MOVIESCENE_API).
        MarkIndex = MovieScene->AddMarkedFrame(MarkedFrame);
    }

    const int32 MarkedFrameCount = MovieScene->GetMarkedFrames().Num();

    // Read back the values the engine actually stored (it auto-generates a label if empty).
    FString StoredLabel;
    bool bStoredFence = bIsDeterminismFence;
    bool bStoredInclusive = bIsInclusive;
    if (MovieScene->GetMarkedFrames().IsValidIndex(MarkIndex))
    {
        const FMovieSceneMarkedFrame& Stored = MovieScene->GetMarkedFrames()[MarkIndex];
        StoredLabel = Stored.Label;
        bStoredFence = Stored.bIsDeterminismFence;
        bStoredInclusive = Stored.bIsInclusiveTime;
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetNumberField(TEXT("frame"), Frame);
    Response->SetNumberField(TEXT("mark_index"), MarkIndex);
    Response->SetStringField(TEXT("label"), StoredLabel);
    Response->SetBoolField(TEXT("is_determinism_fence"), bStoredFence);
    Response->SetBoolField(TEXT("is_inclusive"), bStoredInclusive);
    Response->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Response->SetNumberField(TEXT("marked_frame_count"), MarkedFrameCount);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_marked_frame"));
}

// MCP:get_marked_frames - Lists every user marked frame in a Level Sequence's movie scene.
//   @param sequence_name - Name of the target Level Sequence.
//   @returns { sequence_name: string, marked_frames: [{index, frame, label, is_determinism_fence, is_inclusive}], marked_frame_count: int, marked_frames_locked: bool }

TSharedPtr<FJsonObject> HandleGetMarkedFrames(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }
    const UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(LevelSequenceUtils::FindByName(SequenceName), SequenceName, MovieScene)) { return Failure; }

    // GetMarkedFrames() is an inline const accessor returning the live array (MOVIESCENE_API none needed).
    const TArray<FMovieSceneMarkedFrame>& Marks = MovieScene->GetMarkedFrames();

    TArray<TSharedPtr<FJsonValue>> MarksArray;
    for (int32 Index = 0; Index < Marks.Num(); ++Index)
    {
        const FMovieSceneMarkedFrame& Mark = Marks[Index];
        TSharedPtr<FJsonObject> MarkObj = MakeShared<FJsonObject>();
        MarkObj->SetNumberField(TEXT("index"), Index);
        MarkObj->SetNumberField(TEXT("frame"), Mark.FrameNumber.Value);
        MarkObj->SetStringField(TEXT("label"), Mark.Label);
        MarkObj->SetBoolField(TEXT("is_determinism_fence"), Mark.bIsDeterminismFence);
        MarkObj->SetBoolField(TEXT("is_inclusive"), Mark.bIsInclusiveTime);
        MarksArray.Add(MakeShared<FJsonValueObject>(MarkObj));
    }

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetArrayField(TEXT("marked_frames"), MarksArray);
    Response->SetNumberField(TEXT("marked_frame_count"), Marks.Num());
    Response->SetBoolField(TEXT("marked_frames_locked"), MovieScene->AreMarkedFramesLocked());
    return mcp_common::MakeOkReply(Response);
}

// MCP:remove_marked_frame - Removes one user marked frame from a Level Sequence, by index, frame, or label.
//   @param sequence_name - Name of the target Level Sequence.
//   @param index - Optional mark index to delete. Takes precedence over frame/label.
//   @param frame - Optional tick frame number to resolve the mark to delete (FindMarkedFrameByFrameNumber).
//   @param label - Optional label to resolve the mark to delete (FindMarkedFrameByLabel).
//   @returns { sequence_name: string, removed_index: int, already_removed: bool, marked_frame_count: int, saved: bool }

TSharedPtr<FJsonObject> HandleRemoveMarkedFrame(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }

    // Exactly one selector is required. Resolve to a concrete index below.
    double IndexD = 0.0;
    const bool bHasIndex = Params->TryGetNumberField(TEXT("index"), IndexD);
    double FrameD = 0.0;
    const bool bHasFrame = Params->TryGetNumberField(TEXT("frame"), FrameD);
    FString Label;
    const bool bHasLabel = Params->TryGetStringField(TEXT("label"), Label) && !Label.IsEmpty();
    if (!bHasIndex && !bHasFrame && !bHasLabel)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing selector: provide one of 'index', 'frame', or 'label'"));
    }

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    // Resolve the target index (index > frame > label). FindMarkedFrameBy* return INDEX_NONE when absent.
    int32 TargetIndex = INDEX_NONE;
    if (bHasIndex)
    {
        TargetIndex = static_cast<int32>(FMath::RoundToInt(IndexD));
    }
    else if (bHasFrame)
    {
        TargetIndex = MovieScene->FindMarkedFrameByFrameNumber(FFrameNumber(static_cast<int32>(FMath::RoundToInt(FrameD))));
    }
    else
    {
        TargetIndex = MovieScene->FindMarkedFrameByLabel(Label);
    }

    // Idempotent: removing an already-absent mark is a clean no-op, not an error.
    const bool bValid = MovieScene->GetMarkedFrames().IsValidIndex(TargetIndex);
    bool bSaved = false;
    if (bValid)
    {
        const FScopedTransaction Transaction(LOCTEXT("RemoveMarkedFrame", "MCP: Remove Marked Frame"));
        MovieScene->Modify();
        MovieScene->DeleteMarkedFrame(TargetIndex);
        bSaved = SaveSequencePackage(Sequence);
    }

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetNumberField(TEXT("removed_index"), bValid ? TargetIndex : -1);
    Response->SetBoolField(TEXT("already_removed"), !bValid);
    Response->SetNumberField(TEXT("marked_frame_count"), MovieScene->GetMarkedFrames().Num());
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("remove_marked_frame"));
}

// MCP:clear_marked_frames - Removes ALL user marked frames from a Level Sequence's movie scene.
//   @param sequence_name - Name of the target Level Sequence.
//   @returns { sequence_name: string, removed_count: int, already_empty: bool, marked_frame_count: int, saved: bool }

TSharedPtr<FJsonObject> HandleClearMarkedFrames(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }
    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    const int32 RemovedCount = MovieScene->GetMarkedFrames().Num();
    // Idempotent: clearing an already-empty set is a clean no-op.
    const bool bAlreadyEmpty = RemovedCount == 0;
    bool bSaved = false;
    if (!bAlreadyEmpty)
    {
        const FScopedTransaction Transaction(LOCTEXT("ClearMarkedFrames", "MCP: Clear Marked Frames"));
        MovieScene->Modify();
        MovieScene->DeleteMarkedFrames();
        bSaved = SaveSequencePackage(Sequence);
    }

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetNumberField(TEXT("removed_count"), RemovedCount);
    Response->SetBoolField(TEXT("already_empty"), bAlreadyEmpty);
    Response->SetNumberField(TEXT("marked_frame_count"), MovieScene->GetMarkedFrames().Num());
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("clear_marked_frames"));
}

// MCP:set_marked_frames_locked - Sets whether a Level Sequence's user marked frames are locked.
//   @param sequence_name - Name of the target Level Sequence.
//   @param locked - Bool: whether marked frames should be locked.
//   @returns { sequence_name: string, marked_frames_locked: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetMarkedFramesLocked(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }

    bool bLocked = false;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredBool(Params, TEXT("locked"), bLocked)) { return Missing; }

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    // Overwrite-setter (naturally idempotent). SetMarkedFramesLocked / AreMarkedFramesLocked
    // are both MOVIESCENE_API.
    const FScopedTransaction Transaction(LOCTEXT("SetMarkedFramesLocked", "MCP: Set Marked Frames Locked"));
    MovieScene->Modify();
    MovieScene->SetMarkedFramesLocked(bLocked);
    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetBoolField(TEXT("marked_frames_locked"), MovieScene->AreMarkedFramesLocked());
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_marked_frames_locked"));
}

// MCP:set_playback_range_locked - Sets whether a Level Sequence's playback range is locked (UMovieScene::SetPlaybackRangeLocked).  Overwrite-setter (naturally idempotent); the locked flag is read back via IsPlaybackRangeLocked().
//   @param sequence_name - Name of the target Level Sequence.
//   @param locked - Bool: whether the playback range should be locked.
//   @returns { sequence_name: string, locked: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetPlaybackRangeLocked(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }

    bool bLocked = false;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredBool(Params, TEXT("locked"), bLocked)) { return Missing; }

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    // Overwrite-setter (naturally idempotent). SetPlaybackRangeLocked /
    // IsPlaybackRangeLocked are both MOVIESCENE_API (MovieScene.h).
    const FScopedTransaction Transaction(LOCTEXT("SetPlaybackRangeLocked", "MCP: Set Playback Range Locked"));
    MovieScene->Modify();
    MovieScene->SetPlaybackRangeLocked(bLocked);
    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetBoolField(TEXT("locked"), MovieScene->IsPlaybackRangeLocked());
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_playback_range_locked"));
}

// ---------------------------------------------------------------------------
// Root tracks: event / fade
// ---------------------------------------------------------------------------

// MCP:add_event_track_to_sequence - Adds a root event track (UMovieSceneEventTrack) to a Level Sequence.
//   @param sequence_name - Name of the target Level Sequence.
//   @returns { sequence_name: string, track_class: string, section_count: int, saved: bool }

TSharedPtr<FJsonObject> HandleAddEventTrackToSequence(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }
    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    const FScopedTransaction Transaction(LOCTEXT("AddEventTrack", "MCP: Add Event Track"));
    MovieScene->Modify();

    // Root event track -- AddTrack<UMovieSceneEventTrack>() verified MOVIESCENE_API.
    UMovieSceneEventTrack* Track = MovieScene->AddTrack<UMovieSceneEventTrack>();
    if (!Track)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to add event track"));
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
    Response->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_event_track_to_sequence"));
}

// MCP:add_fade_track_to_sequence - Adds (or returns the existing) root fade track with a fade section spanning the playback range.
//   @param sequence_name - Name of the target Level Sequence.
//   @returns { sequence_name: string, track_class: string, track_added: bool, section_added: bool, section_count: int, section_class: string, saved: bool }

TSharedPtr<FJsonObject> HandleAddFadeTrackToSequence(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }
    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    const FScopedTransaction Transaction(LOCTEXT("AddFadeTrack", "MCP: Add Fade Track"));
    MovieScene->Modify();

    // Root fade track -- idempotent find-or-create. FindTrack/AddTrack<UMovieSceneFadeTrack>()
    // verified MOVIESCENE_API; re-running must not stack duplicate fade tracks.
    bool bTrackCreated = false;
    UMovieSceneFadeTrack* Track = UEAgentSequencerImpl::FindOrAddTrack<UMovieSceneFadeTrack>(MovieScene, bTrackCreated);
    if (!Track)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to add fade track"));
    }

    // Add a fade section spanning the playback range, but only when the track has
    // none yet (idempotent: re-running must not stack duplicate sections).
    // UMovieSceneFadeTrack::CreateNewSection returns a UMovieSceneFadeSection;
    // AddSection is the property-track override (exported).
    bool bSectionAdded = false;
    UMovieSceneSection* Section = UEAgentSequencerImpl::FirstOrNewPlaybackSection<UMovieSceneSection>(MovieScene, Track, bSectionAdded);

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
    Response->SetBoolField(TEXT("track_added"), bTrackCreated);
    Response->SetBoolField(TEXT("section_added"), bSectionAdded);
    Response->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
    if (Section)
    {
        Response->SetStringField(TEXT("section_class"), Section->GetClass()->GetName());
    }
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_fade_track_to_sequence"));
}

// ---------------------------------------------------------------------------
// Property / skeletal-animation / audio tracks
// ---------------------------------------------------------------------------

// Normalize the requested property type to a canonical lowercase token.
static FString NormalizePropertyType(const FString& InType)
{
    const FString Lower = InType.ToLower();
    if (Lower == TEXT("float") || Lower == TEXT("scalar")) { return TEXT("float"); }
    if (Lower == TEXT("bool") || Lower == TEXT("boolean")) { return TEXT("bool"); }
    if (Lower == TEXT("vector") || Lower == TEXT("vector3")) { return TEXT("vector"); }
    if (Lower == TEXT("color") || Lower == TEXT("linearcolor")) { return TEXT("color"); }
    if (Lower == TEXT("visibility") || Lower == TEXT("visible")) { return TEXT("visibility"); }
    return Lower;
}

namespace UEAgentSequencerImpl
{
    // The binding's track for a normalized property type; returns the error reply for an unsupported type.
    TSharedPtr<FJsonObject> FindPropertyTrack(UMovieScene* MovieScene, const FGuid& BindingGuid, const FString& PropertyType,
        const FString& PropertyTypeRaw, UMovieScenePropertyTrack*& OutTrack)
    {
        if (PropertyType == TEXT("float"))           { OutTrack = MovieScene->FindTrack<UMovieSceneFloatTrack>(BindingGuid); }
        else if (PropertyType == TEXT("bool"))        { OutTrack = MovieScene->FindTrack<UMovieSceneBoolTrack>(BindingGuid); }
        else if (PropertyType == TEXT("vector"))      { OutTrack = MovieScene->FindTrack<UMovieSceneFloatVectorTrack>(BindingGuid); }
        else if (PropertyType == TEXT("color"))       { OutTrack = MovieScene->FindTrack<UMovieSceneColorTrack>(BindingGuid); }
        else if (PropertyType == TEXT("visibility"))  { OutTrack = MovieScene->FindTrack<UMovieSceneVisibilityTrack>(BindingGuid); }
        else
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Unsupported property_type '%s' (use Float|Bool|Vector|Color|Visibility)"), *PropertyTypeRaw));
        }
        return nullptr;
    }

    // The first section of the binding's property track; returns the error reply for an unsupported type or no section.
    TSharedPtr<FJsonObject> FindPropertySection(UMovieScene* MovieScene, const FGuid& BindingGuid, const FString& PropertyType,
        const FString& PropertyTypeRaw, UMovieSceneSection*& OutSection)
    {
        UMovieScenePropertyTrack* PropTrack = nullptr;
        if (TSharedPtr<FJsonObject> Failure = FindPropertyTrack(MovieScene, BindingGuid, PropertyType, PropertyTypeRaw, PropTrack))
        {
            return Failure;
        }
        if (!PropTrack)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("No %s property track on this binding. Call add_property_track first."), *PropertyTypeRaw));
        }
        if (PropTrack->GetAllSections().Num() == 0)
        {
            return mcp_common::MakeErrorReply(TEXT("Property track has no section"));
        }
        OutSection = PropTrack->GetAllSections()[0];
        if (!OutSection)
        {
            return mcp_common::MakeErrorReply(TEXT("Property track section is null"));
        }
        return nullptr;
    }
}

// MCP:add_property_track - Adds a typed property track + section to a Level Sequence binding.  The track animates a named property on the bound object. A section spanning the movie scene's playback range is created so keys evaluate.
//   @param sequence_name - Name of the target Level Sequence (found by AssetName under /Game).
//   @param binding_guid - GUID of an existing binding (possessable or spawnable).
//   @param property_type - One of "Float", "Bool", "Vector", "Color", "Visibility".
//   @param property_name - Name of the animated property (e.g. "RelativeLocation", "Intensity", "bHidden"). Used as both the property name and path.
//   @returns { sequence_name: string, binding_guid: string, property_type: string, property_name: string, track_class: string, section_class: string, section_created: bool, section_count: int, saved: bool }

TSharedPtr<FJsonObject> HandleAddPropertyTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName, GuidStr;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequenceAndBinding(Params, SequenceName, GuidStr)) { return Missing; }

    FString PropertyTypeRaw;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property_type"), PropertyTypeRaw)) { return Missing; }
    const FString PropertyType = NormalizePropertyType(PropertyTypeRaw);

    FString PropertyName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property_name"), PropertyName)) { return Missing; }

    FGuid BindingGuid;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentSequencerImpl::ParseGuid(GuidStr, TEXT("binding_guid"), BindingGuid)) { return Invalid; }

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    // Confirm the binding exists (possessable, spawnable, or otherwise registered).
    if (!UEAgentSequencerImpl::IsKnownBinding(MovieScene, BindingGuid))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Binding '%s' not found in sequence"), *GuidStr));
    }

    const FScopedTransaction Transaction(LOCTEXT("AddPropertyTrack", "MCP: Add Property Track"));
    MovieScene->Modify();

    // Add the typed property track. All of these derive from UMovieScenePropertyTrack,
    // so the common SetPropertyNameAndPath + CreateNewSection path applies.
    // UMovieSceneFloatVectorTrack additionally needs SetNumChannelsUsed before the
    // section is created (the section reads NumChannelsUsed to size its channel proxy).
    UMovieScenePropertyTrack* PropTrack = nullptr;
    int32 VectorChannels = 0;
    if (PropertyType == TEXT("float"))
    {
        PropTrack = MovieScene->AddTrack<UMovieSceneFloatTrack>(BindingGuid);
    }
    else if (PropertyType == TEXT("bool"))
    {
        PropTrack = MovieScene->AddTrack<UMovieSceneBoolTrack>(BindingGuid);
    }
    else if (PropertyType == TEXT("vector"))
    {
        UMovieSceneFloatVectorTrack* VecTrack = MovieScene->AddTrack<UMovieSceneFloatVectorTrack>(BindingGuid);
        if (VecTrack)
        {
            VectorChannels = 3; // FVector property -> 3 channels (XYZ)
            VecTrack->SetNumChannelsUsed(VectorChannels);
        }
        PropTrack = VecTrack;
    }
    else if (PropertyType == TEXT("color"))
    {
        PropTrack = MovieScene->AddTrack<UMovieSceneColorTrack>(BindingGuid);
    }
    else if (PropertyType == TEXT("visibility"))
    {
        PropTrack = MovieScene->AddTrack<UMovieSceneVisibilityTrack>(BindingGuid);
    }
    else
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unsupported property_type '%s' (use Float|Bool|Vector|Color|Visibility)"), *PropertyTypeRaw));
    }

    if (!PropTrack)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to add %s property track"), *PropertyTypeRaw));
    }

    // The animated property name (and path == name for simple top-level properties).
    PropTrack->SetPropertyNameAndPath(FName(*PropertyName), PropertyName);

    // Add a section spanning the playback range so keys evaluate.
    bool bSectionCreated = false;
    UMovieSceneSection* Section = (PropTrack->GetAllSections().Num() > 0) ? PropTrack->GetAllSections()[0] : nullptr;
    if (!Section)
    {
        Section = PropTrack->CreateNewSection();
        if (Section)
        {
            // Size the float-vector section's channel proxy to match the track.
            if (UMovieSceneFloatVectorSection* VecSection = Cast<UMovieSceneFloatVectorSection>(Section))
            {
                VecSection->SetChannelsUsed(VectorChannels > 0 ? VectorChannels : 3);
            }
            Section->SetRange(MovieScene->GetPlaybackRange());
            PropTrack->AddSection(*Section);
            bSectionCreated = true;
        }
    }
    if (!Section)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create property section"));
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("binding_guid"), GuidStr);
    Response->SetStringField(TEXT("property_type"), PropertyType);
    Response->SetStringField(TEXT("property_name"), PropertyName);
    Response->SetStringField(TEXT("track_class"), PropTrack->GetClass()->GetName());
    Response->SetStringField(TEXT("section_class"), Section->GetClass()->GetName());
    Response->SetBoolField(TEXT("section_created"), bSectionCreated);
    Response->SetNumberField(TEXT("section_count"), PropTrack->GetAllSections().Num());
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_property_track"));
}

// MCP:add_keyframe_to_property_track - Adds a keyframe to a property track's section channel(s) at the given time.  The property track must already exist (call add_property_track first). The value is keyed onto every channel of the section:
//   @param sequence_name - Name of the target Level Sequence.
//   @param binding_guid - GUID of the binding owning the property track.
//   @param property_type - One of "Float", "Bool", "Vector", "Color", "Visibility".
//   @param time_seconds - Time (seconds, >= 0) at which to place the key.
//   @param value - Key value. For Float: a scalar. For Bool/Visibility: a bool. For Vector/Color: a list of numbers (Vector/Color). Defaults to 0 / [] if omitted.
//   @returns { sequence_name: string, binding_guid: string, property_type: string, time_seconds: float, key_frame: int, keys_added: int, channel_count: int, saved: bool }

TSharedPtr<FJsonObject> HandleAddKeyframeToPropertyTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName, GuidStr;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequenceAndBinding(Params, SequenceName, GuidStr)) { return Missing; }

    FString PropertyTypeRaw;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property_type"), PropertyTypeRaw)) { return Missing; }
    const FString PropertyType = NormalizePropertyType(PropertyTypeRaw);

    double TimeSeconds = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("time_seconds"), TimeSeconds)) { return Missing; }
    if (TimeSeconds < 0.0)
    {
        return mcp_common::MakeErrorReply(TEXT("time_seconds must be >= 0.0"));
    }

    FGuid BindingGuid;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentSequencerImpl::ParseGuid(GuidStr, TEXT("binding_guid"), BindingGuid)) { return Invalid; }

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    // Resolve the matching property track for the requested type.
    UMovieSceneSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::FindPropertySection(MovieScene, BindingGuid, PropertyType, PropertyTypeRaw, Section))
    {
        return Failure;
    }

    const FScopedTransaction Transaction(LOCTEXT("AddPropertyKey", "MCP: Add Property Keyframe"));
    Section->Modify();

    // Convert seconds -> tick frame and ensure the section range covers the key.
    const FFrameRate TickResolution = MovieScene->GetTickResolution();
    const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
    UEAgentSequencerImpl::ExtendSectionToFrame(Section, KeyFrame);

    // Read the scalar value, and a vector array (for Vector / Color types).
    double ScalarValue = 0.0;
    Params->TryGetNumberField(TEXT("value"), ScalarValue);

    TArray<double> VectorValues;
    if (Params->HasField(TEXT("value")))
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Params->TryGetArrayField(TEXT("value"), Arr) && Arr)
        {
            for (const TSharedPtr<FJsonValue>& V : *Arr)
            {
                if (V.IsValid()) { VectorValues.Add(V->AsNumber()); }
            }
        }
    }

    int32 KeysAdded = 0;

    if (PropertyType == TEXT("bool") || PropertyType == TEXT("visibility"))
    {
        // Bool / visibility sections expose a single FMovieSceneBoolChannel.
        TArrayView<FMovieSceneBoolChannel*> BoolChannels =
            Section->GetChannelProxy().GetChannels<FMovieSceneBoolChannel>();
        const bool bValue = (ScalarValue != 0.0) || (VectorValues.Num() > 0 && VectorValues[0] != 0.0);
        for (FMovieSceneBoolChannel* Channel : BoolChannels)
        {
            if (Channel)
            {
                Channel->GetData().UpdateOrAddKey(KeyFrame, bValue);
                ++KeysAdded;
            }
        }
    }
    else
    {
        // Float / Vector / Color sections all expose FMovieSceneFloatChannel(s):
        //   float   -> 1 channel  (value)
        //   vector  -> N channels (X,Y,Z[,W])
        //   color   -> 4 channels (R,G,B,A)
        TArrayView<FMovieSceneFloatChannel*> FloatChannels =
            Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();

        for (int32 i = 0; i < FloatChannels.Num(); ++i)
        {
            FMovieSceneFloatChannel* Channel = FloatChannels[i];
            if (!Channel) { continue; }

            float ChannelValue = 0.0f;
            if (PropertyType == TEXT("float"))
            {
                ChannelValue = static_cast<float>(ScalarValue);
            }
            else // vector / color
            {
                if (VectorValues.IsValidIndex(i))
                {
                    ChannelValue = static_cast<float>(VectorValues[i]);
                }
                else
                {
                    ChannelValue = static_cast<float>(ScalarValue);
                }
            }

            Channel->GetData().UpdateOrAddKey(KeyFrame, FMovieSceneFloatValue(ChannelValue));
            ++KeysAdded;
        }
    }

    if (KeysAdded == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No keyable channels found on the property section"));
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("binding_guid"), GuidStr);
    Response->SetStringField(TEXT("property_type"), PropertyType);
    Response->SetNumberField(TEXT("time_seconds"), TimeSeconds);
    Response->SetNumberField(TEXT("key_frame"), KeyFrame.Value);
    Response->SetNumberField(TEXT("keys_added"), KeysAdded);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_keyframe_to_property_track"));
}

// MCP:add_skeletal_animation_track - Adds a skeletal-animation track to a Level Sequence binding.  If anim_path loads a UAnimSequenceBase (a UAnimMontage qualifies), a section is added starting at the playback range start; otherwise just the empty track.
//   @param sequence_name - Name of the target Level Sequence.
//   @param binding_guid - GUID of an existing binding (typically a SkeletalMeshActor).
//   @param anim_path - Optional content path of a UAnimSequenceBase or UAnimMontage, e.g. "/Game/Anims/MyAnim".
//   @returns { sequence_name: string, binding_guid: string, track_class: string, track_added: bool, anim_loaded: bool, section_added: bool, section_count: int, anim_path: string, saved: bool }

TSharedPtr<FJsonObject> HandleAddSkeletalAnimationTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName, GuidStr;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequenceAndBinding(Params, SequenceName, GuidStr)) { return Missing; }

    FGuid BindingGuid;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentSequencerImpl::ParseGuid(GuidStr, TEXT("binding_guid"), BindingGuid)) { return Invalid; }

    FString AnimPath;
    Params->TryGetStringField(TEXT("anim_path"), AnimPath);

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    if (!UEAgentSequencerImpl::IsKnownBinding(MovieScene, BindingGuid))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Binding '%s' not found in sequence"), *GuidStr));
    }

    const FScopedTransaction Transaction(LOCTEXT("AddSkelAnimTrack", "MCP: Add Skeletal Animation Track"));
    MovieScene->Modify();

    bool bTrackCreated = false;
    UMovieSceneSkeletalAnimationTrack* Track =
        UEAgentSequencerImpl::FindOrAddTrack<UMovieSceneSkeletalAnimationTrack>(MovieScene, BindingGuid, bTrackCreated);
    if (!Track)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to add skeletal animation track"));
    }

    // Optionally load a UAnimSequenceBase (UAnimMontage / UAnimSequence both qualify)
    // and add a section starting at the playback range start.
    bool bAnimLoaded = false;
    bool bSectionAdded = false;
    if (!AnimPath.IsEmpty())
    {
        UAnimSequenceBase* AnimSeq = LoadObject<UAnimSequenceBase>(nullptr, *AnimPath);
        if (AnimSeq)
        {
            bAnimLoaded = true;
            const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
            const FFrameNumber Start = PlaybackRange.GetLowerBound().IsClosed()
                ? PlaybackRange.GetLowerBoundValue() : FFrameNumber(0);
            // AddNewAnimation(FFrameNumber, UAnimSequenceBase*) -- verified on the
            // UMovieSceneCommonAnimationTrack base; delegates to AddNewAnimationOnRow.
            UMovieSceneSection* Section = Track->AddNewAnimation(Start, AnimSeq);
            bSectionAdded = (Section != nullptr);
        }
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("binding_guid"), GuidStr);
    Response->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
    Response->SetBoolField(TEXT("track_added"), bTrackCreated);
    Response->SetBoolField(TEXT("anim_loaded"), bAnimLoaded);
    Response->SetBoolField(TEXT("section_added"), bSectionAdded);
    Response->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
    if (!AnimPath.IsEmpty())
    {
        Response->SetStringField(TEXT("anim_path"), AnimPath);
    }
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_skeletal_animation_track"));
}

// MCP:add_audio_section - Adds (or reuses) a root audio track on a Level Sequence and optionally a section.  Finds an existing root UMovieSceneAudioTrack or adds one. If sound_path loads a USoundBase, a sound section is placed at the playback range start.
//   @param sequence_name - Name of the target Level Sequence.
//   @param sound_path - Optional content path of a USoundBase, e.g. "/Game/Audio/MySound".
//   @returns { sequence_name: string, track_class: string, audio_track: string, audio_track_index: int (the index set_audio_track_volume / set_audio_track_pitch take), track_added: bool, sound_loaded: bool, section_added: bool, section_count: int, sound_path: string, saved: bool }

TSharedPtr<FJsonObject> HandleAddAudioSection(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }
    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    FString SoundPath;
    Params->TryGetStringField(TEXT("sound_path"), SoundPath);

    const FScopedTransaction Transaction(LOCTEXT("AddAudioSection", "MCP: Add Audio Section"));
    MovieScene->Modify();

    // Find or add a root audio track (FindTrack<>() with no guid scans the root tracks).
    bool bTrackCreated = false;
    UMovieSceneAudioTrack* Track = UEAgentSequencerImpl::FindOrAddTrack<UMovieSceneAudioTrack>(MovieScene, bTrackCreated);
    if (!Track)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to add/find root audio track"));
    }

    // Optionally load a USoundBase and place a section at the playback range start.
    bool bSoundLoaded = false;
    bool bSectionAdded = false;
    if (!SoundPath.IsEmpty())
    {
        USoundBase* Sound = LoadObject<USoundBase>(nullptr, *SoundPath);
        if (Sound)
        {
            bSoundLoaded = true;
            const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
            const FFrameNumber Start = PlaybackRange.GetLowerBound().IsClosed()
                ? PlaybackRange.GetLowerBoundValue() : FFrameNumber(0);
            // AddNewSound has a check(Sound) inside -- only called with a valid sound.
            UMovieSceneSection* Section = Track->AddNewSound(Sound, Start);
            bSectionAdded = (Section != nullptr);
        }
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
    // audio_track = the resolved audio track's stable identifier (FName).
    // audio_track_index = the track's position among the root AUDIO tracks, the index
    // set_audio_track_volume / set_audio_track_pitch take.
    Response->SetStringField(TEXT("audio_track"), Track->GetFName().ToString());
    int32 AudioTrackIndex = INDEX_NONE;
    int32 AudioSeen = 0;
    for (UMovieSceneTrack* RootTrack : MovieScene->GetTracks())
    {
        if (RootTrack == Track)
        {
            AudioTrackIndex = AudioSeen;
            break;
        }
        if (RootTrack && RootTrack->IsA<UMovieSceneAudioTrack>())
        {
            ++AudioSeen;
        }
    }
    Response->SetNumberField(TEXT("audio_track_index"), AudioTrackIndex);
    Response->SetBoolField(TEXT("track_added"), bTrackCreated);
    Response->SetBoolField(TEXT("sound_loaded"), bSoundLoaded);
    Response->SetBoolField(TEXT("section_added"), bSectionAdded);
    Response->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
    if (!SoundPath.IsEmpty())
    {
        Response->SetStringField(TEXT("sound_path"), SoundPath);
    }
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_audio_section"));
}

// ---------------------------------------------------------------------------
// Subsequence / shot tracks + component binding
// ---------------------------------------------------------------------------

// Resolve a ULevelSequence from a full /Game object path OR a bare asset name; a bare name, or a path that
// does not load, falls back to the recursive AssetName scan of LevelSequenceUtils::FindByName.
ULevelSequence* LoadSequenceByPathOrName(const FString& PathOrName)
{
    if (PathOrName.IsEmpty())
    {
        return nullptr;
    }
    // Full /Game path -> load directly.
    if (PathOrName.StartsWith(TEXT("/")))
    {
        if (ULevelSequence* Loaded = Cast<ULevelSequence>(
                StaticLoadObject(ULevelSequence::StaticClass(), nullptr, *PathOrName)))
        {
            return Loaded;
        }
        // Path that didn't load -- fall back to scanning by the trailing asset name.
        FString AssetName = PathOrName;
        int32 SlashIdx = INDEX_NONE;
        if (PathOrName.FindLastChar(TEXT('/'), SlashIdx))
        {
            AssetName = PathOrName.RightChop(SlashIdx + 1);
        }
        int32 DotIdx = INDEX_NONE;
        if (AssetName.FindChar(TEXT('.'), DotIdx))
        {
            AssetName = AssetName.Left(DotIdx);
        }
        return LevelSequenceUtils::FindByName(AssetName);
    }
    // Bare asset name -> recursive AssetName scan under /Game.
    return LevelSequenceUtils::FindByName(PathOrName);
}

// Load an inner ULevelSequence by full /Game path, creating + persisting a fresh
// one (NewObject + Initialize, like ULevelSequenceFactoryNew) if it does not exist.
ULevelSequence* LoadOrCreateInnerSequence(const FString& InnerPath, FString* OutError)
{
    if (InnerPath.IsEmpty())
    {
        return nullptr;
    }

    // Normalize: strip a trailing ".Object" suffix if present, ensure leading /Game.
    FString PackagePath = InnerPath;
    int32 DotIdx = INDEX_NONE;
    if (PackagePath.FindChar(TEXT('.'), DotIdx))
    {
        PackagePath = PackagePath.Left(DotIdx);
    }
    if (!PackagePath.StartsWith(TEXT("/")))
    {
        PackagePath = FString::Printf(TEXT("/Game/%s"), *PackagePath);
    }

    // Already on disk / in memory -> load (idempotent).
    if (ULevelSequence* Existing = Cast<ULevelSequence>(
            StaticLoadObject(ULevelSequence::StaticClass(), nullptr, *PackagePath)))
    {
        return Existing;
    }

    // Derive the asset (object) name from the trailing path component.
    FString AssetName = PackagePath;
    int32 SlashIdx = INDEX_NONE;
    if (PackagePath.FindLastChar(TEXT('/'), SlashIdx))
    {
        AssetName = PackagePath.RightChop(SlashIdx + 1);
    }
    if (AssetName.IsEmpty())
    {
        return nullptr;
    }
    if (const FString AssetConflict = mcp_common::AssetNameConflict(PackagePath, AssetName, ULevelSequence::StaticClass()); !AssetConflict.IsEmpty())
    {
        if (OutError)
        {
            *OutError = AssetConflict;
        }
        return nullptr;
    }

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        return nullptr;
    }
    Package->FullyLoad();

    ULevelSequence* NewSequence = NewObject<ULevelSequence>(
        Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
    if (!NewSequence)
    {
        return nullptr;
    }
    // Initialize() creates the inner UMovieScene with sensible defaults.
    NewSequence->Initialize();
    if (!NewSequence->GetMovieScene())
    {
        return nullptr;
    }

    FAssetRegistryModule::AssetCreated(NewSequence);
    SaveSequencePackage(NewSequence);
    return NewSequence;
}

namespace UEAgentSequencerImpl
{
    // A path beside Sequence (same folder, default /Game) named after it plus Suffix.
    FString SiblingSequencePath(const ULevelSequence* Sequence, const TCHAR* Suffix)
    {
        const FString Base = Sequence->GetOutermost()->GetName(); // e.g. /Game/MyGame/SUBSEQ_Example
        int32 SlashIdx = INDEX_NONE;
        FString Folder = TEXT("/Game");
        if (Base.FindLastChar(TEXT('/'), SlashIdx))
        {
            Folder = Base.Left(SlashIdx);
        }
        return FString::Printf(TEXT("%s/%s%s"), *Folder, *Sequence->GetName(), Suffix);
    }
}

// MCP:add_subsequence_track - Nests one Level Sequence in another: finds or adds the target's root subsequence track, adds a sub section playing the inner sequence from start_frame for duration tick-resolution frames, and saves the target. The inner sequence is sub_sequence_path, or <target name>_Sub in the target's folder when omitted; it is loaded when it exists, else created and saved as a new asset. Each call adds another section. If no inner sequence resolves, the track is still saved and section_added is false. For cinematic shots use add_shot_track.
//   @param sequence_path - Full /Game path (e.g. "/Game/MyGame/SUBSEQ_Example") OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param sub_sequence_path - Optional content path of the sub Level Sequence to nest. If omitted, a sibling inner Level Sequence is created and persisted.
//   @param start_frame - Inclusive start frame for the sub section (default 0).
//   @param duration - Section duration in tick frames (default 150).
//   @returns { sequence_path: string, track_class: string, track_added: bool, section_added: bool, inner_sequence_created: bool, inner_sequence_error?: string (why the inner sequence could not be created), start_frame: int, duration: int, section_count: int, sub_sequence_path: string, sub_sequence_name: string, saved: bool }

TSharedPtr<FJsonObject> HandleAddSubsequenceTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequencePathOrName(Params, SequencePath)) { return Missing; }

    FString SubSequencePath;
    Params->TryGetStringField(TEXT("sub_sequence_path"), SubSequencePath);

    double StartFrameD = 0.0;
    Params->TryGetNumberField(TEXT("start_frame"), StartFrameD);
    double DurationD = 150.0;
    Params->TryGetNumberField(TEXT("duration"), DurationD);
    const int32 StartFrame = FMath::RoundToInt(StartFrameD);
    int32 Duration = FMath::RoundToInt(DurationD);
    if (Duration <= 0) { Duration = 150; }

    ULevelSequence* Sequence = LoadSequenceByPathOrName(SequencePath);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequencePath, MovieScene)) { return Failure; }

    const FScopedTransaction Transaction(LOCTEXT("AddSubsequenceTrack", "MCP: Add Subsequence Track"));
    MovieScene->Modify();

    // Root sub track (no object binding). FindTrack<>() / AddTrack<>() are the
    // no-guid overloads on UMovieScene (verified MOVIESCENE_API).
    bool bTrackCreated = false;
    UMovieSceneSubTrack* Track = UEAgentSequencerImpl::FindOrAddTrack<UMovieSceneSubTrack>(MovieScene, bTrackCreated);
    if (!Track)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to add subsequence track"));
    }

    // Resolve / create the inner sequence to nest. If omitted, synthesize a sibling
    // inner sequence under the same folder so the section binds a real sub-sequence.
    bool bInnerCreated = false;
    const FString InnerPath = SubSequencePath.IsEmpty()
        ? UEAgentSequencerImpl::SiblingSequencePath(Sequence, TEXT("_Sub")) : SubSequencePath;

    FString InnerError;
    ULevelSequence* InnerSeq = LoadOrCreateInnerSequence(InnerPath, &InnerError);
    bInnerCreated = (InnerSeq != nullptr && InnerSeq != Sequence);

    bool bSectionAdded = false;
    FString InnerName;
    UMovieSceneSubSection* SubSection = nullptr;
    if (InnerSeq)
    {
        // AddSequence forwards to the exported AddSequenceOnRow(MOVIESCENE_API).
        SubSection = Track->AddSequence(InnerSeq, FFrameNumber(StartFrame), Duration);
        if (SubSection)
        {
            bSectionAdded = true;
            InnerName = InnerSeq->GetName();
        }
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
    Response->SetBoolField(TEXT("track_added"), bTrackCreated);
    Response->SetBoolField(TEXT("section_added"), bSectionAdded);
    Response->SetBoolField(TEXT("inner_sequence_created"), bInnerCreated);
    if (!InnerError.IsEmpty())
    {
        Response->SetStringField(TEXT("inner_sequence_error"), InnerError);
    }
    Response->SetNumberField(TEXT("start_frame"), StartFrame);
    Response->SetNumberField(TEXT("duration"), Duration);
    Response->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
    if (InnerSeq)
    {
        Response->SetStringField(TEXT("sub_sequence_path"), InnerSeq->GetPathName());
        Response->SetStringField(TEXT("sub_sequence_name"), InnerName);
    }
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_subsequence_track"));
}

// MCP:add_shot_track - Adds a cinematic shot track (UMovieSceneCinematicShotTrack) to a Level Sequence.
//   @param sequence_path - Full /Game path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @returns { sequence_path: string, track_class: string, track_added: bool, section_count: int, saved: bool }

TSharedPtr<FJsonObject> HandleAddShotTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequencePathOrName(Params, SequencePath)) { return Missing; }

    ULevelSequence* Sequence = LoadSequenceByPathOrName(SequencePath);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequencePath, MovieScene)) { return Failure; }

    const FScopedTransaction Transaction(LOCTEXT("AddShotTrack", "MCP: Add Cinematic Shot Track"));
    MovieScene->Modify();

    bool bTrackCreated = false;
    UMovieSceneCinematicShotTrack* Track = UEAgentSequencerImpl::FindOrAddTrack<UMovieSceneCinematicShotTrack>(MovieScene, bTrackCreated);
    if (!Track)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to add cinematic shot track"));
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
    Response->SetBoolField(TEXT("track_added"), bTrackCreated);
    Response->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_shot_track"));
}

// MCP:add_shot_section - Adds a cinematic shot section (UMovieSceneCinematicShotSection) to the shot track.  Creates the cinematic shot track if it does not exist yet.
//   @param sequence_path - Full /Game path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param shot_sequence_path - Optional content path of the inner shot Level Sequence. If omitted, an inner shot Level Sequence is created and persisted.
//   @param start_frame - Inclusive start frame for the shot section (default 0).
//   @param duration - Section duration in tick frames (default 150).
//   @returns { sequence_path: string, track_class: string, track_added: bool, section_added: bool, inner_sequence_created: bool, shot_name: string, shot_sequence_path: string, start_frame: int, duration: int, section_count: int, section_class: string, saved: bool }

TSharedPtr<FJsonObject> HandleAddShotSection(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequencePathOrName(Params, SequencePath)) { return Missing; }

    FString ShotSequencePath;
    Params->TryGetStringField(TEXT("shot_sequence_path"), ShotSequencePath);

    double StartFrameD = 0.0;
    Params->TryGetNumberField(TEXT("start_frame"), StartFrameD);
    double DurationD = 150.0;
    Params->TryGetNumberField(TEXT("duration"), DurationD);
    const int32 StartFrame = FMath::RoundToInt(StartFrameD);
    int32 Duration = FMath::RoundToInt(DurationD);
    if (Duration <= 0) { Duration = 150; }

    ULevelSequence* Sequence = LoadSequenceByPathOrName(SequencePath);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequencePath, MovieScene)) { return Failure; }

    const FScopedTransaction Transaction(LOCTEXT("AddShotSection", "MCP: Add Cinematic Shot Section"));
    MovieScene->Modify();

    // Ensure the cinematic shot track exists.
    bool bTrackCreated = false;
    UMovieSceneCinematicShotTrack* Track = UEAgentSequencerImpl::FindOrAddTrack<UMovieSceneCinematicShotTrack>(MovieScene, bTrackCreated);
    if (!Track)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to obtain cinematic shot track"));
    }

    // Resolve / create the inner shot sequence.
    bool bInnerCreated = false;
    const FString InnerPath = ShotSequencePath.IsEmpty()
        ? UEAgentSequencerImpl::SiblingSequencePath(Sequence, TEXT("_Shot")) : ShotSequencePath;
    FString InnerError;
    ULevelSequence* ShotSeq = LoadOrCreateInnerSequence(InnerPath, &InnerError);
    bInnerCreated = (ShotSeq != nullptr && ShotSeq != Sequence);
    if (!ShotSeq)
    {
        return mcp_common::MakeErrorReply(InnerError.IsEmpty()
            ? FString(TEXT("Failed to load or create the inner shot sequence")) : InnerError);
    }

    // AddSequence forwards to the exported AddSequenceOnRow (MOVIESCENETRACKS_API).
    UMovieSceneSubSection* SubSection = Track->AddSequence(ShotSeq, FFrameNumber(StartFrame), Duration);
    UMovieSceneCinematicShotSection* ShotSection = Cast<UMovieSceneCinematicShotSection>(SubSection);
    const bool bSectionAdded = (SubSection != nullptr);

    FString ShotName;
    if (ShotSection)
    {
        ShotName = ShotSection->GetShotDisplayName();
    }
    if (ShotName.IsEmpty())
    {
        ShotName = ShotSeq->GetName();
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
    Response->SetBoolField(TEXT("track_added"), bTrackCreated);
    Response->SetBoolField(TEXT("section_added"), bSectionAdded);
    Response->SetBoolField(TEXT("inner_sequence_created"), bInnerCreated);
    Response->SetStringField(TEXT("shot_name"), ShotName);
    Response->SetStringField(TEXT("shot_sequence_path"), ShotSeq->GetPathName());
    Response->SetNumberField(TEXT("start_frame"), StartFrame);
    Response->SetNumberField(TEXT("duration"), Duration);
    Response->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
    if (SubSection)
    {
        Response->SetStringField(TEXT("section_class"), SubSection->GetClass()->GetName());
    }
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_shot_section"));
}

// MCP:bind_component_to_sequence - Adds a possessable binding for an actor's component to a Level Sequence.  Resolves the actor by label in the current editor world, finds the named component (by component name, class name, or "root"), adds a possessable for the component class, binds the component, and parents the component binding to the actor's binding when the actor is already possessed.
//   @param sequence_path - Full /Game path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param actor_name - Label (or name) of the actor in the current editor world.
//   @param component_name - Component object name, component class name, or "root".
//   @returns { sequence_path: string, actor_name: string, component_name: string, component_class: string, parent_binding_guid: string, binding_guid: string, display_name: string, possessable: bool, saved: bool }

TSharedPtr<FJsonObject> HandleBindComponentToSequence(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequencePathOrName(Params, SequencePath)) { return Missing; }
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }
    FString ComponentName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("component_name"), ComponentName)) { return Missing; }

    ULevelSequence* Sequence = LoadSequenceByPathOrName(SequencePath);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequencePath, MovieScene)) { return Failure; }

    UWorld* World = nullptr;
    AActor* TargetActor = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::FindActorInEditorWorld(ActorName, World, TargetActor))
    {
        return Failure;
    }

    // Resolve the component by name (component object name OR class name). Falls back
    // to the root component if "root"/"rootcomponent" requested.
    UActorComponent* TargetComponent = nullptr;
    const FString WantedLower = ComponentName.ToLower();
    if (WantedLower == TEXT("root") || WantedLower == TEXT("rootcomponent"))
    {
        TargetComponent = TargetActor->GetRootComponent();
    }
    if (!TargetComponent)
    {
        for (UActorComponent* Comp : TargetActor->GetComponents())
        {
            if (!Comp) { continue; }
            if (Comp->GetName() == ComponentName ||
                Comp->GetName().ToLower() == WantedLower ||
                Comp->GetClass()->GetName() == ComponentName)
            {
                TargetComponent = Comp;
                break;
            }
        }
    }
    // Last resort: if the actor's root component class matches the requested name.
    if (!TargetComponent && TargetActor->GetRootComponent() &&
        TargetActor->GetRootComponent()->GetClass()->GetName() == ComponentName)
    {
        TargetComponent = TargetActor->GetRootComponent();
    }
    if (!TargetComponent)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Component '%s' not found on actor '%s'"), *ComponentName, *ActorName));
    }

    const FScopedTransaction Transaction(LOCTEXT("BindComponent", "MCP: Bind Component To Sequence"));
    MovieScene->Modify();
    Sequence->Modify();

    // Add a possessable for the COMPONENT and bind it (Actor is the playback context).
    const FString DisplayName = FString::Printf(TEXT("%s.%s"), *TargetActor->GetActorLabel(), *TargetComponent->GetName());
    const FGuid ComponentGuid = MovieScene->AddPossessable(DisplayName, TargetComponent->GetClass());
    if (!ComponentGuid.IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to add component possessable"));
    }
    // BindPossessableObject(ObjectId, PossessedObject, Context) -- verified LEVELSEQUENCE_API.
    Sequence->BindPossessableObject(ComponentGuid, *TargetComponent, TargetActor);

    // If the actor is already bound, parent the component possessable to it so the
    // component binding resolves under the actor. FindPossessableObjectId is the
    // non-deprecated lookup on the UMovieSceneSequence base (verified MOVIESCENE_API).
    bool bParented = false;
    FString ParentGuidStr;
    const FGuid ActorGuid = Sequence->FindPossessableObjectId(*TargetActor, World);
    if (ActorGuid.IsValid())
    {
        if (FMovieScenePossessable* Possessable = MovieScene->FindPossessable(ComponentGuid))
        {
            // Use the non-deprecated overload that takes the owning UMovieScene*.
            Possessable->SetParent(ActorGuid, MovieScene);
            bParented = true;
            ParentGuidStr = ActorGuid.ToString();
        }
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetStringField(TEXT("actor_name"), TargetActor->GetActorLabel());
    Response->SetStringField(TEXT("component_name"), TargetComponent->GetName());
    Response->SetStringField(TEXT("component_class"), TargetComponent->GetClass()->GetName());
    Response->SetStringField(TEXT("binding_guid"), ComponentGuid.ToString());
    Response->SetStringField(TEXT("display_name"), MovieScene->GetObjectDisplayName(ComponentGuid).ToString());
    Response->SetBoolField(TEXT("component_bound"), true);
    Response->SetBoolField(TEXT("parented_to_actor"), bParented);
    if (!ParentGuidStr.IsEmpty())
    {
        Response->SetStringField(TEXT("parent_binding_guid"), ParentGuidStr);
    }
    Response->SetNumberField(TEXT("possessable_count"), MovieScene->GetPossessableCount());
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("bind_component_to_sequence"));
}

// ---------------------------------------------------------------------------
// Section easing / completion mode / blend type / info
// ---------------------------------------------------------------------------

namespace UEAgentSequencerImpl
{
    // The track's section at SectionIndex into OutSection; the error reply saying why there is none (a null entry
    // gets an error reply with an empty message).
    TSharedPtr<FJsonObject> SectionAtIndex(const UMovieSceneTrack* Track, int32 SectionIndex, UMovieSceneSection*& OutSection)
    {
        const TArray<UMovieSceneSection*> Sections = Track->GetAllSections();
        if (Sections.Num() == 0)
        {
            return mcp_common::MakeErrorReply(TEXT("Track has no sections"));
        }
        if (!Sections.IsValidIndex(SectionIndex))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("section_index %d out of range (%d sections)"), SectionIndex, Sections.Num()));
        }
        OutSection = Sections[SectionIndex];
        if (OutSection == nullptr)
        {
            return mcp_common::MakeErrorReply(FString());
        }
        return nullptr;
    }
}

// Helper: resolve a UMovieSceneSection from the common param set (sequence_path/sequence_name, binding_guid,
// track_type or else track_index, section_index) into OutSequence and OutSection. Returns the error reply on
// failure.
TSharedPtr<FJsonObject> ResolveSectionFromParams(
    const TSharedPtr<FJsonObject>& Params, ULevelSequence*& OutSequence, UMovieSceneSection*& OutSection)
{
    OutSequence = nullptr;
    OutSection = nullptr;

    // 1. Resolve sequence
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequencePath(Params, SequencePath))
    {
        return Missing;
    }

    OutSequence = LoadSequenceByPathOrName(SequencePath);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(OutSequence, SequencePath, MovieScene))
    {
        return Failure;
    }

    // 2. Resolve binding GUID
    FString GuidStr;
    if (!Params->TryGetStringField(TEXT("binding_guid"), GuidStr) || GuidStr.IsEmpty())
    {
        Params->TryGetStringField(TEXT("binding_name_or_guid"), GuidStr);
    }
    if (GuidStr.IsEmpty())
    {
        return mcp_common::MissingParameterReply(TEXT("binding_guid"));
    }

    FGuid BindingGuid;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentSequencerImpl::ParseGuid(GuidStr, TEXT("binding_guid"), BindingGuid))
    {
        return Invalid;
    }

    // 3. Resolve track -- by track_type class filter, falling back to track_index
    FString TrackTypeRaw;
    Params->TryGetStringField(TEXT("track_type"), TrackTypeRaw);
    const FString TrackTypeLower = TrackTypeRaw.ToLower();

    int32 SectionIndex = 0;
    Params->TryGetNumberField(TEXT("section_index"), SectionIndex);

    // Get all tracks for this binding
    const TArray<UMovieSceneTrack*> AllTracks = MovieScene->FindTracks(UMovieSceneTrack::StaticClass(), BindingGuid);
    if (AllTracks.Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("No tracks found for the specified binding"));
    }

    UMovieSceneTrack* Track = nullptr;
    if (!TrackTypeLower.IsEmpty())
    {
        // Map track_type string to UClass filter
        UClass* TrackClass = nullptr;
        if (TrackTypeLower == TEXT("transform") || TrackTypeLower == TEXT("3dtransform"))
        {
            TrackClass = UMovieScene3DTransformTrack::StaticClass();
        }
        else if (TrackTypeLower == TEXT("float") || TrackTypeLower == TEXT("scalar"))
        {
            TrackClass = UMovieSceneFloatTrack::StaticClass();
        }
        else if (TrackTypeLower == TEXT("bool") || TrackTypeLower == TEXT("boolean"))
        {
            TrackClass = UMovieSceneBoolTrack::StaticClass();
        }
        else if (TrackTypeLower == TEXT("vector"))
        {
            TrackClass = UMovieSceneFloatVectorTrack::StaticClass();
        }
        else if (TrackTypeLower == TEXT("color"))
        {
            TrackClass = UMovieSceneColorTrack::StaticClass();
        }
        else if (TrackTypeLower == TEXT("visibility"))
        {
            TrackClass = UMovieSceneVisibilityTrack::StaticClass();
        }
        else if (TrackTypeLower == TEXT("audio"))
        {
            TrackClass = UMovieSceneAudioTrack::StaticClass();
        }
        else if (TrackTypeLower == TEXT("event"))
        {
            TrackClass = UMovieSceneEventTrack::StaticClass();
        }
        else if (TrackTypeLower == TEXT("fade"))
        {
            TrackClass = UMovieSceneFadeTrack::StaticClass();
        }
        else if (TrackTypeLower == TEXT("skeletalmesh") || TrackTypeLower == TEXT("skeletalmeshanimation") || TrackTypeLower == TEXT("skeletalanimation"))
        {
            TrackClass = UMovieSceneSkeletalAnimationTrack::StaticClass();
        }
        else if (TrackTypeLower == TEXT("subsequence") || TrackTypeLower == TEXT("sub"))
        {
            TrackClass = UMovieSceneSubTrack::StaticClass();
        }
        else if (TrackTypeLower == TEXT("cameracut"))
        {
            TrackClass = UMovieSceneCameraCutTrack::StaticClass();
        }
        else if (TrackTypeLower == TEXT("property"))
        {
            TrackClass = UMovieScenePropertyTrack::StaticClass();
        }
        else if (TrackTypeLower == TEXT("attach") || TrackTypeLower == TEXT("3dattach"))
        {
            TrackClass = UMovieScene3DAttachTrack::StaticClass();
        }

        if (TrackClass)
        {
            // Find first track matching the class
            for (UMovieSceneTrack* T : AllTracks)
            {
                if (T && T->IsA(TrackClass))
                {
                    Track = T;
                    break;
                }
            }
            if (!Track)
            {
                return mcp_common::MakeErrorReply(FString::Printf(TEXT("No track of type '%s' found on binding"), *TrackTypeRaw));
            }
        }
    }

    // Fallback: use track_index
    if (!Track)
    {
        int32 TrackIndex = 0;
        Params->TryGetNumberField(TEXT("track_index"), TrackIndex);
        if (!AllTracks.IsValidIndex(TrackIndex))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("track_index %d out of range (%d tracks)"), TrackIndex, AllTracks.Num()));
        }
        Track = AllTracks[TrackIndex];
    }

    if (!Track)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to resolve track"));
    }

    return UEAgentSequencerImpl::SectionAtIndex(Track, SectionIndex, OutSection);
}

// Helper: convert EMovieSceneCompletionMode to string
static FString CompletionModeToString(EMovieSceneCompletionMode Mode)
{
    switch (Mode)
    {
    case EMovieSceneCompletionMode::KeepState:     return TEXT("keep_state");
    case EMovieSceneCompletionMode::RestoreState:  return TEXT("restore_state");
    case EMovieSceneCompletionMode::ProjectDefault: return TEXT("project_default");
    default: return TEXT("unknown");
    }
}

// Helper: convert string to EMovieSceneCompletionMode
static bool StringToCompletionMode(const FString& Str, EMovieSceneCompletionMode& OutMode)
{
    const FString Lower = Str.ToLower();
    if (Lower == TEXT("keep_state") || Lower == TEXT("keepstate"))
    {
        OutMode = EMovieSceneCompletionMode::KeepState;
        return true;
    }
    if (Lower == TEXT("restore_state") || Lower == TEXT("restorestate"))
    {
        OutMode = EMovieSceneCompletionMode::RestoreState;
        return true;
    }
    if (Lower == TEXT("project_default") || Lower == TEXT("projectdefault"))
    {
        OutMode = EMovieSceneCompletionMode::ProjectDefault;
        return true;
    }
    return false;
}

// Helper: convert EMovieSceneBlendType to string
static FString BlendTypeToString(EMovieSceneBlendType Type)
{
    switch (Type)
    {
    case EMovieSceneBlendType::Absolute:       return TEXT("absolute");
    case EMovieSceneBlendType::Additive:       return TEXT("additive");
    case EMovieSceneBlendType::Relative:       return TEXT("relative");
    case EMovieSceneBlendType::AdditiveFromBase: return TEXT("additive_from_base");
    case EMovieSceneBlendType::Override:       return TEXT("override");
    case EMovieSceneBlendType::Invalid:        return TEXT("invalid");
    default: return TEXT("unknown");
    }
}

// Helper: convert string to EMovieSceneBlendType
static bool StringToBlendType(const FString& Str, EMovieSceneBlendType& OutType)
{
    const FString Lower = Str.ToLower();
    if (Lower == TEXT("absolute"))
    {
        OutType = EMovieSceneBlendType::Absolute;
        return true;
    }
    if (Lower == TEXT("additive"))
    {
        OutType = EMovieSceneBlendType::Additive;
        return true;
    }
    if (Lower == TEXT("relative"))
    {
        OutType = EMovieSceneBlendType::Relative;
        return true;
    }
    if (Lower == TEXT("additive_from_base") || Lower == TEXT("additivefrombase"))
    {
        OutType = EMovieSceneBlendType::AdditiveFromBase;
        return true;
    }
    if (Lower == TEXT("override"))
    {
        OutType = EMovieSceneBlendType::Override;
        return true;
    }
    return false;
}

// MCP:set_section_easing - Set ease-in/ease-out durations (in frames) on a sequencer section.
//   @param sequence_path - Full /Game path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param binding_guid - GUID of the binding whose section to edit.
//   @param track_type - Optional class filter ("transform"|"float"|"bool"|"vector"|"color"|"visibility"|"audio"|"event"|"fade"|"skeletalanimation"|"subsequence"|"cameracut"|"property").
//   @param track_index - Index into the binding's tracks (0 = first track, default 0; used when track_type is omitted).
//   @param section_index - Index of the section on the resolved track (default 0).
//   @param use_manual - bool, use Manual* ease durations instead of Auto* (default true).
//   @param ease_in_frames - Ease-in duration in frames (omit to leave unchanged).
//   @param ease_out_frames - Ease-out duration in frames (omit to leave unchanged).
//   @param binding_name_or_guid - Alias for binding_guid, read only when binding_guid is absent; it must also be a GUID string, a bound object's name is not resolved.
//   @returns { sequence_path: string, section_index: int, ease_in_frames: int, ease_out_frames: int, manual_ease_in: bool, manual_ease_out: bool, set: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetSectionEasing(const TSharedPtr<FJsonObject>& Params)
{
    ULevelSequence* Sequence = nullptr;
    UMovieSceneSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ResolveSectionFromParams(Params, Sequence, Section)) { return Failure; }

    bool bUseManual = true;
    Params->TryGetBoolField(TEXT("use_manual"), bUseManual);

    double EaseInD = -1.0, EaseOutD = -1.0;
    Params->TryGetNumberField(TEXT("ease_in_frames"), EaseInD);
    Params->TryGetNumberField(TEXT("ease_out_frames"), EaseOutD);

    const FScopedTransaction Transaction(LOCTEXT("SetSectionEasing", "MCP: Set Section Easing"));
    Section->Modify();

    if (bUseManual)
    {
        if (EaseInD >= 0.0)
        {
            Section->Easing.bManualEaseIn = true;
            Section->Easing.ManualEaseInDuration = FMath::RoundToInt(EaseInD);
        }
        if (EaseOutD >= 0.0)
        {
            Section->Easing.bManualEaseOut = true;
            Section->Easing.ManualEaseOutDuration = FMath::RoundToInt(EaseOutD);
        }
    }
    else
    {
        if (EaseInD >= 0.0)
        {
            Section->Easing.bManualEaseIn = false;
            Section->Easing.AutoEaseInDuration = FMath::RoundToInt(EaseInD);
        }
        if (EaseOutD >= 0.0)
        {
            Section->Easing.bManualEaseOut = false;
            Section->Easing.AutoEaseOutDuration = FMath::RoundToInt(EaseOutD);
        }
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetNumberField(TEXT("section_index"), 0);
    Response->SetNumberField(TEXT("ease_in_frames"), Section->Easing.GetEaseInDuration());
    Response->SetNumberField(TEXT("ease_out_frames"), Section->Easing.GetEaseOutDuration());
    Response->SetBoolField(TEXT("manual_ease_in"), Section->Easing.bManualEaseIn);
    Response->SetBoolField(TEXT("manual_ease_out"), Section->Easing.bManualEaseOut);
    Response->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_section_easing"));
}

// MCP:set_section_completion_mode - Set how a sequencer section handles state when no longer evaluated (keep_state|restore_state|project_default).
//   @param sequence_path - Full /Game path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param binding_guid - GUID of the binding whose section to edit.
//   @param track_type - Optional class filter (see set_section_easing).
//   @param track_index - Index into the binding's tracks (0 = first track, default 0).
//   @param section_index - Index of the section on the resolved track (default 0).
//   @param completion_mode - One of "keep_state", "restore_state", "project_default".
//   @param binding_name_or_guid - Alias for binding_guid, read only when binding_guid is absent; it must also be a GUID string, a bound object's name is not resolved.
//   @returns { sequence_path: string, completion_mode: string, set: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetSectionCompletionMode(const TSharedPtr<FJsonObject>& Params)
{
    ULevelSequence* Sequence = nullptr;
    UMovieSceneSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ResolveSectionFromParams(Params, Sequence, Section)) { return Failure; }

    FString ModeStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("completion_mode"), ModeStr)) { return Missing; }

    EMovieSceneCompletionMode NewMode;
    if (!StringToCompletionMode(ModeStr, NewMode))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid completion_mode '%s' (use keep_state|restore_state|project_default)"), *ModeStr));
    }

    const FScopedTransaction Transaction(LOCTEXT("SetSectionCompletionMode", "MCP: Set Section Completion Mode"));
    Section->Modify();
    Section->SetCompletionMode(NewMode);

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetStringField(TEXT("completion_mode"), CompletionModeToString(Section->GetCompletionMode()));
    Response->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_section_completion_mode"));
}

// MCP:set_section_blend_type - Set the blend type on a sequencer section (absolute|additive|relative|additive_from_base|override).
//   @param sequence_path - Full /Game path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param binding_guid - GUID of the binding whose section to edit.
//   @param track_type - Optional class filter (see set_section_easing).
//   @param track_index - Index into the binding's tracks (0 = first track, default 0).
//   @param section_index - Index of the section on the resolved track (default 0).
//   @param blend_type - One of "absolute", "additive", "relative", "additive_from_base", "override".
//   @param binding_name_or_guid - Alias for binding_guid, read only when binding_guid is absent; it must also be a GUID string, a bound object's name is not resolved.
//   @returns { sequence_path: string, blend_type: string, blend_type_valid: bool, set: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetSectionBlendType(const TSharedPtr<FJsonObject>& Params)
{
    ULevelSequence* Sequence = nullptr;
    UMovieSceneSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ResolveSectionFromParams(Params, Sequence, Section)) { return Failure; }

    FString BlendStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blend_type"), BlendStr)) { return Missing; }

    EMovieSceneBlendType NewBlend;
    if (!StringToBlendType(BlendStr, NewBlend))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid blend_type '%s' (use absolute|additive|relative|additive_from_base|override)"), *BlendStr));
    }

    const FScopedTransaction Transaction(LOCTEXT("SetSectionBlendType", "MCP: Set Section Blend Type"));
    Section->Modify();
    Section->SetBlendType(NewBlend);

    const bool bSaved = SaveSequencePackage(Sequence);

    FOptionalMovieSceneBlendType OptBlend = Section->GetBlendType();

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetStringField(TEXT("blend_type"), BlendTypeToString(OptBlend.BlendType));
    Response->SetBoolField(TEXT("blend_type_valid"), OptBlend.bIsValid);
    Response->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_section_blend_type"));
}

// MCP:get_section_info - Get detailed info about a sequencer section: range, easing, completion mode, blend type, lock/infinite state.
//   @param sequence_path - Full /Game path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param binding_guid - GUID of the binding whose section to inspect.
//   @param track_type - Optional class filter (see set_section_easing).
//   @param track_index - Index into the binding's tracks (0 = first track, default 0).
//   @param section_index - Index of the section on the resolved track (default 0).
//   @param binding_name_or_guid - Alias for binding_guid, read only when binding_guid is absent; it must also be a GUID string, a bound object's name is not resolved.
//   @returns { sequence_path: string, section_index: int, start_frame: int, end_frame: int, ease_in_frames: int, ease_out_frames: int, manual_ease_in: bool, manual_ease_out: bool, completion_mode: string, blend_type: string, blend_type_valid: bool, is_locked: bool, is_active: bool, is_attach_section: bool, attach_rules: object? }

TSharedPtr<FJsonObject> HandleGetSectionInfo(const TSharedPtr<FJsonObject>& Params)
{
    ULevelSequence* Sequence = nullptr;
    UMovieSceneSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ResolveSectionFromParams(Params, Sequence, Section)) { return Failure; }

    int32 SectionIndex = 0;
    Params->TryGetNumberField(TEXT("section_index"), SectionIndex);

    const TRange<FFrameNumber> Range = Section->GetRange();
    const int32 StartFrame = Range.GetLowerBound().IsClosed() ? Range.GetLowerBoundValue().Value : 0;
    const int32 EndFrame = Range.GetUpperBound().IsClosed() ? Range.GetUpperBoundValue().Value : 0;

    FOptionalMovieSceneBlendType OptBlend = Section->GetBlendType();

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetNumberField(TEXT("section_index"), SectionIndex);
    Response->SetNumberField(TEXT("start_frame"), StartFrame);
    Response->SetNumberField(TEXT("end_frame"), EndFrame);
    Response->SetNumberField(TEXT("ease_in_frames"), Section->Easing.GetEaseInDuration());
    Response->SetNumberField(TEXT("ease_out_frames"), Section->Easing.GetEaseOutDuration());
    Response->SetBoolField(TEXT("manual_ease_in"), Section->Easing.bManualEaseIn);
    Response->SetBoolField(TEXT("manual_ease_out"), Section->Easing.bManualEaseOut);
    Response->SetStringField(TEXT("completion_mode"), CompletionModeToString(Section->GetCompletionMode()));
    Response->SetStringField(TEXT("blend_type"), OptBlend.bIsValid ? BlendTypeToString(OptBlend.BlendType) : TEXT("invalid"));
    Response->SetBoolField(TEXT("blend_type_valid"), OptBlend.bIsValid);
    Response->SetBoolField(TEXT("is_locked"), Section->IsLocked());
    Response->SetBoolField(TEXT("is_active"), Section->IsActive());

    // If this is a 3D-attach section, report its 6 attach/detach rules.
    const UMovieScene3DAttachSection* AttachSec = Cast<UMovieScene3DAttachSection>(Section);
    Response->SetBoolField(TEXT("is_attach_section"), AttachSec != nullptr);
    if (AttachSec)
    {
        Response->SetObjectField(TEXT("attach_rules"), AttachRulesJson(AttachSec));
    }
    return mcp_common::MakeOkReply(Response);
}

// ---------------------------------------------------------------------------
// set_section_active / set_section_locked
// The writers of the is_active / is_locked flags get_section_info reads. Both
// are inline UMovieSceneSection setters on public uint32:1 fields. The
// section is resolved like get_section_info (binding_guid + track_type/index +
// section_index). NOTE: SetIsActive uses TryModify(), a no-op if the section is
// LOCKED -- so the response reports the actual is_active read back. Lock LAST.
// ---------------------------------------------------------------------------

// MCP:set_section_active - Toggle whether a sequencer section is active (evaluated) or muted.  Maps to UMovieSceneSection::SetIsActive; reports the actual is_active read back (SetIsActive is a no-op on a LOCKED section).
//   @param sequence_path - Full /Game path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param binding_guid - GUID of the binding whose section to edit.
//   @param track_type - Optional class filter (see set_section_easing).
//   @param track_index - Index into the binding's tracks (0 = first track, default 0).
//   @param section_index - Index of the section on the resolved track (default 0).
//   @param active [bool] - Desired active state (true = evaluated, false = muted).
//   @param binding_name_or_guid - Alias for binding_guid, read only when binding_guid is absent; it must also be a GUID string, a bound object's name is not resolved.
//   @returns { sequence_path: string, section_index: int, is_active: bool, set: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetSectionActive(const TSharedPtr<FJsonObject>& Params)
{
    ULevelSequence* Sequence = nullptr;
    UMovieSceneSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ResolveSectionFromParams(Params, Sequence, Section)) { return Failure; }

    bool bActive = true;
    if (!Params->TryGetBoolField(TEXT("active"), bActive))
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: active (bool)"));

    int32 SectionIndex = 0;
    Params->TryGetNumberField(TEXT("section_index"), SectionIndex);

    const FScopedTransaction Transaction(LOCTEXT("SetSectionActive", "MCP: Set Section Active"));
    Section->Modify();
    Section->SetIsActive(bActive);
    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetNumberField(TEXT("section_index"), SectionIndex);
    Response->SetBoolField(TEXT("is_active"), Section->IsActive());
    Response->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_section_active"));
}

// MCP:set_section_locked - Lock or unlock a sequencer section.  Maps to UMovieSceneSection::SetIsLocked; a locked section cannot be modified by other edit ops, so lock LAST in an edit chain.
//   @param sequence_path - Full /Game path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param binding_guid - GUID of the binding whose section to edit.
//   @param track_type - Optional class filter (see set_section_easing).
//   @param track_index - Index into the binding's tracks (0 = first track, default 0).
//   @param section_index - Index of the section on the resolved track (default 0).
//   @param locked [bool] - Desired locked state.
//   @param binding_name_or_guid - Alias for binding_guid, read only when binding_guid is absent; it must also be a GUID string, a bound object's name is not resolved.
//   @returns { sequence_path: string, section_index: int, is_locked: bool, set: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetSectionLocked(const TSharedPtr<FJsonObject>& Params)
{
    ULevelSequence* Sequence = nullptr;
    UMovieSceneSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ResolveSectionFromParams(Params, Sequence, Section)) { return Failure; }

    bool bLocked = false;
    if (!Params->TryGetBoolField(TEXT("locked"), bLocked))
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: locked (bool)"));

    int32 SectionIndex = 0;
    Params->TryGetNumberField(TEXT("section_index"), SectionIndex);

    const FScopedTransaction Transaction(LOCTEXT("SetSectionLocked", "MCP: Set Section Locked"));
    Section->Modify();
    Section->SetIsLocked(bLocked);
    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetNumberField(TEXT("section_index"), SectionIndex);
    Response->SetBoolField(TEXT("is_locked"), Section->IsLocked());
    Response->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_section_locked"));
}

// ---------------------------------------------------------------------------
// Generic section removal, one delete for what every per-track add_*_section
// adds: resolve the section exactly like get_section_info /
// set_section_active (binding_guid + track_type/index + section_index), climb to
// its owning track via GetTypedOuter<UMovieSceneTrack>(), then RemoveSectionAt on
// the TRACK (the track owns the Sections array, so Modify() the track, not the
// section). An unresolvable address is an honest error (matching the set_section_*
// siblings -- NOT a silent no-op). RemoveSectionAt has an internal IsValidIndex
// guard, but the resolver has already validated the index before we get here.
// ---------------------------------------------------------------------------

// MCP:remove_section - Remove a section from a sequencer track by index.  Resolves the section like set_section_active (binding_guid + track_type/index + section_index), then removes it from its owning track via UMovieSceneTrack::RemoveSectionAt. One delete for what every per-track add_*_section adds.
//   @param sequence_path - Full /Game path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param binding_guid - GUID of the binding whose section to remove.
//   @param track_type - Optional class filter (see set_section_easing).
//   @param track_index - Index into the binding's tracks (0 = first track, default 0).
//   @param section_index - Index of the section on the resolved track to remove (default 0).
//   @param binding_name_or_guid - Alias for binding_guid, read only when binding_guid is absent; it must also be a GUID string, a bound object's name is not resolved.
//   @returns { sequence_path: string, section_index: int, track_type: string, track_index: int, removed: bool, remaining_sections: int, saved: bool }
TSharedPtr<FJsonObject> HandleRemoveSection(const TSharedPtr<FJsonObject>& Params)
{
    ULevelSequence* Sequence = nullptr;
    UMovieSceneSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ResolveSectionFromParams(Params, Sequence, Section)) { return Failure; }

    // The track owns the Sections array; climb to it from the resolved section.
    UMovieSceneTrack* Track = Section->GetTypedOuter<UMovieSceneTrack>();
    if (!Track)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Resolved section has no owning UMovieSceneTrack"));
    }

    int32 SectionIndex = 0;
    Params->TryGetNumberField(TEXT("section_index"), SectionIndex);

    FString TrackTypeRaw;
    Params->TryGetStringField(TEXT("track_type"), TrackTypeRaw);
    int32 TrackIndex = 0;
    Params->TryGetNumberField(TEXT("track_index"), TrackIndex);

    UE_LOG(LogTemp, Verbose,
        TEXT("[remove_section] seq='%s' binding track_type='%s' track_index=%d section_index=%d sections_before=%d"),
        *Sequence->GetPathName(), *TrackTypeRaw, TrackIndex, SectionIndex, Track->GetAllSections().Num());

    const FScopedTransaction Transaction(LOCTEXT("RemoveSection", "MCP: Remove Section"));
    Track->Modify();
    Track->RemoveSectionAt(SectionIndex);
    const int32 RemainingSections = Track->GetAllSections().Num();
    const bool bSaved = SaveSequencePackage(Sequence);

    UE_LOG(LogTemp, Verbose,
        TEXT("[remove_section] removed section_index=%d -> remaining_sections=%d saved=%d"),
        SectionIndex, RemainingSections, bSaved ? 1 : 0);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetNumberField(TEXT("section_index"), SectionIndex);
    Response->SetStringField(TEXT("track_type"), TrackTypeRaw);
    Response->SetNumberField(TEXT("track_index"), TrackIndex);
    Response->SetBoolField(TEXT("removed"), true);
    Response->SetNumberField(TEXT("remaining_sections"), RemainingSections);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("remove_section"));
}

// ---------------------------------------------------------------------------
// Spawnables / possessables (advanced)
// ---------------------------------------------------------------------------

// Helper: resolve a component class from a friendly name.
static UClass* ResolveComponentClass(const FString& ClassName)
{
    const FString Lower = ClassName.ToLower();
    if (Lower == TEXT("staticmeshcomponent") || Lower == TEXT("staticmesh"))
        return UStaticMeshComponent::StaticClass();
    if (Lower == TEXT("spotlightcomponent") || Lower == TEXT("spotlight"))
        return USpotLightComponent::StaticClass();
    if (Lower == TEXT("pointlightcomponent") || Lower == TEXT("pointlight"))
        return UPointLightComponent::StaticClass();
    if (Lower == TEXT("sceneComponent") || Lower == TEXT("scenecomponent"))
        return USceneComponent::StaticClass();

    if (ClassName.Contains(TEXT(".")) || ClassName.StartsWith(TEXT("/Script/")))
    {
        if (UClass* Loaded = LoadObject<UClass>(nullptr, *ClassName))
            return Loaded;
    }
    if (UClass* Found = UClass::TryFindTypeSlow<UClass>(ClassName))
        return Found;
    if (UClass* First = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::None))
        return First;
    return nullptr;
}

// MCP:add_subcomponent_possessable - Adds a component possessable as a child of an existing binding (possessable or spawnable) of a Level Sequence found by asset name under /Game, in an undo transaction, and saves the sequence; each call adds a new one, named possessable_name or else the component_class text. It records name, class and parent only: no component object is bound to it. To possess an actual component of an editor-world actor use bind_component_to_sequence; for whole actors use bind_actor_to_sequence or add_spawnable_to_sequence. An unknown parent GUID or an unresolvable class is an error.
//   @param sequence_name - Name of the level sequence asset.
//   @param parent_binding_guid - GUID of the parent binding (spawnable or possessable).
//   @param component_class - Component class name (e.g. 'StaticMeshComponent', 'SpotLightComponent').
//   @param possessable_name - Optional override name for the new possessable.
//   @returns { sequence_name: string, possessable_guid: string, component_class: string, parent_guid: string, saved: bool }

TSharedPtr<FJsonObject> HandleAddSubcomponentPossessable(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }

    FString ParentGuidStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parent_binding_guid"), ParentGuidStr)) { return Missing; }

    FGuid ParentGuid;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentSequencerImpl::ParseGuid(ParentGuidStr, TEXT("parent_binding_guid"), ParentGuid)) { return Invalid; }

    FString ComponentClassName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("component_class"), ComponentClassName)) { return Missing; }

    UClass* CompClass = ResolveComponentClass(ComponentClassName);
    if (!CompClass)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve component class '%s'"), *ComponentClassName));

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    // Verify parent exists
    if (!MovieScene->FindPossessable(ParentGuid))
    {
        const bool bKnown = const_cast<const UMovieScene*>(MovieScene)->GetBindings().ContainsByPredicate(
            [&](const FMovieSceneBinding& B){ return B.GetObjectGuid() == ParentGuid; });
        if (!bKnown)
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Parent binding '%s' not found"), *ParentGuidStr));
    }

    const FScopedTransaction Transaction(LOCTEXT("AddSubcomponentPossessable", "MCP: Add Subcomponent Possessable"));
    MovieScene->Modify();

    FString PossessableName = ComponentClassName;
    Params->TryGetStringField(TEXT("possessable_name"), PossessableName);

    const FGuid NewGuid = MovieScene->AddPossessable(PossessableName, CompClass);
    if (!NewGuid.IsValid())
        return mcp_common::MakeErrorReply(TEXT("Failed to add possessable to MovieScene"));

    // Set parent
    FMovieScenePossessable* Poss = MovieScene->FindPossessable(NewGuid);
    if (Poss)
        Poss->SetParent(ParentGuid, MovieScene);

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("possessable_guid"), NewGuid.ToString());
    Response->SetStringField(TEXT("component_class"), CompClass->GetName());
    Response->SetStringField(TEXT("parent_guid"), ParentGuidStr);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_subcomponent_possessable"));
}

// MCP:set_spawn_ownership - Set the ESpawnOwnership lifetime policy of a spawnable custom binding.
//   @param sequence_name - Name of the level sequence asset (or sequence_path).
//   @param sequence_path - Asset path of the level sequence (alias for sequence_name).
//   @param binding_guid - GUID of the spawnable binding (from add_spawnable_from_class / get_sequence_spawnables).
//   @param ownership - One of: InnerSequence | RootSequence | External.
//   @returns { sequence_name: string, binding_guid: string, ownership: string, saved: bool }

TSharedPtr<FJsonObject> HandleSetSpawnOwnership(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if ((!Params->TryGetStringField(TEXT("sequence_name"), SequenceName) || SequenceName.IsEmpty()) &&
        (!Params->TryGetStringField(TEXT("sequence_path"), SequenceName) || SequenceName.IsEmpty()))
    {
        return mcp_common::MissingParameterReply(TEXT("sequence_name"));
    }

    FString GuidStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("binding_guid"), GuidStr)) { return Missing; }

    FString OwnershipStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ownership"), OwnershipStr)) { return Missing; }

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    FGuid BindingGuid;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentSequencerImpl::ParseGuid(GuidStr, TEXT("binding_guid"), BindingGuid)) { return Invalid; }

    // Binding references live on the SEQUENCE (ULevelSequence override is the real impl;
    // the UMovieSceneSequence base returns nullptr). Non-const overload for the non-const
    // GetCustomBinding accessor below.
    // ULevelSequence overrides only the const GetBindingReferences(), which name-hides the
    // base's non-const overload -- so call it through the base type to get the mutable refs
    // (the base non-const impl just const_casts the virtual's real result).
    FMovieSceneBindingReferences* Refs = static_cast<UMovieSceneSequence*>(Sequence)->GetBindingReferences();
    if (!Refs)
        return mcp_common::MakeErrorReply(TEXT("Sequence has no binding references"));

    // Headless-safe: resolve the custom binding by GUID + index 0 (no default) and cast
    // directly to the spawnable binding base to read/write its public SpawnOwnership field.
    // We deliberately do NOT use FMovieSceneSpawnable / AsSpawnable, which require a playback
    // state and return const, so they are not authorable headlessly.
    UMovieSceneCustomBinding* CB = Refs->GetCustomBinding(BindingGuid, 0);
    UMovieSceneSpawnableBindingBase* SB = Cast<UMovieSceneSpawnableBindingBase>(CB);
    if (!SB)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Binding '%s' is not a spawnable custom binding"), *GuidStr));

    // Parse ownership string -> ESpawnOwnership (accept the bare enumerator name, the
    // "ESpawnOwnership::"-qualified form, and "MasterSequence" as an alias for RootSequence).
    FString OwnershipName = OwnershipStr;
    if (OwnershipName.Equals(TEXT("MasterSequence"), ESearchCase::IgnoreCase))
        OwnershipName = TEXT("RootSequence");

    UEnum* OwnershipEnum = StaticEnum<ESpawnOwnership>();
    int64 Value = OwnershipEnum ? OwnershipEnum->GetValueByNameString(OwnershipName) : INDEX_NONE;
    if (Value == INDEX_NONE && OwnershipEnum)
        Value = OwnershipEnum->GetValueByNameString(FString::Printf(TEXT("ESpawnOwnership::%s"), *OwnershipName));
    if (Value == INDEX_NONE)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid ownership '%s'. Must be one of: InnerSequence | RootSequence | External"), *OwnershipStr));

    const FScopedTransaction Transaction(LOCTEXT("SetSpawnOwnership", "MCP: Set Spawn Ownership"));
    SB->Modify();
    MovieScene->Modify();
    SB->SpawnOwnership = static_cast<ESpawnOwnership>(Value);

    const bool bSaved = SaveSequencePackage(Sequence);

    // Read-back: re-read the field and emit its enumerator name.
    const FString ReadBack = OwnershipEnum
        ? OwnershipEnum->GetNameStringByValue(static_cast<int64>(SB->SpawnOwnership))
        : OwnershipName;

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("binding_guid"), GuidStr);
    Response->SetStringField(TEXT("ownership"), ReadBack);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_spawn_ownership"));
}

// MCP:get_sequence_spawnables - List all spawnables in a level sequence with GUID, name, template class.
//   @param sequence_name - Name of the level sequence asset.
//   @returns { sequence_name: string, spawnables: [{guid, name, template_class, spawnable, spawn_ownership}], spawnable_count: int }

TSharedPtr<FJsonObject> HandleGetSequenceSpawnables(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }
    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    const UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    TArray<TSharedPtr<FJsonValue>> SpawnablesArray;

    // Binding references (for per-spawnable spawn_ownership read-back). May be null.
    // ULevelSequence overrides only the const GetBindingReferences(), which name-hides the
    // base's non-const overload -- so call it through the base type to get the mutable refs
    // (the base non-const impl just const_casts the virtual's real result).
    FMovieSceneBindingReferences* Refs = static_cast<UMovieSceneSequence*>(Sequence)->GetBindingReferences();
    UEnum* OwnershipEnum = StaticEnum<ESpawnOwnership>();

    // Spawnables are custom bindings recorded as possessables + a UMovieSceneSpawnTrack
    // (NOT the deprecated UMovieScene::Spawnables[] array, which CreateSpawnable does not
    // populate). A binding is therefore a spawnable iff a spawn track exists for its GUID
    // -- this is the same detection used by HandleGetSequenceBindings. Enumerating
    // GetSpawnableCount()/GetSpawnable(i) here would return 0 for bindings created via the
    // add_spawnable_from_class / add_spawnable_to_sequence path.
    for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
    {
        const FGuid Guid = Binding.GetObjectGuid();
        if (MovieScene->FindTrack<UMovieSceneSpawnTrack>(Guid) == nullptr)
            continue;

        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("guid"), Guid.ToString());
        Obj->SetStringField(TEXT("name"), MovieScene->GetObjectDisplayName(Guid).ToString());

        // The spawnable custom binding records the bound class via AddPossessable(name, class),
        // so the possessable entry carries the spawned actor class.
        const FMovieScenePossessable* Poss = const_cast<UMovieScene*>(MovieScene)->FindPossessable(Guid);
        const UClass* BoundClass = Poss ? Poss->GetPossessedObjectClass() : nullptr;
        Obj->SetStringField(TEXT("template_class"), BoundClass ? BoundClass->GetName() : TEXT("null"));
        Obj->SetBoolField(TEXT("spawnable"), true);

        // Emit the ESpawnOwnership of the spawnable custom binding (empty if
        // this binding has no spawnable custom binding to cast to).
        FString OwnershipName;
        if (Refs)
        {
            if (UMovieSceneSpawnableBindingBase* SB = Cast<UMovieSceneSpawnableBindingBase>(Refs->GetCustomBinding(Guid, 0)))
                OwnershipName = OwnershipEnum ? OwnershipEnum->GetNameStringByValue(static_cast<int64>(SB->SpawnOwnership)) : FString();
        }
        Obj->SetStringField(TEXT("spawn_ownership"), OwnershipName);

        SpawnablesArray.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetArrayField(TEXT("spawnables"), SpawnablesArray);
    Response->SetNumberField(TEXT("spawnable_count"), SpawnablesArray.Num());
    return mcp_common::MakeOkReply(Response);
}

// MCP:get_sequence_possessables - List the possessable bindings of a Level Sequence found by asset name under /Game (first match): GUID, name, possessed class and parent GUID of each, plus possessable_count. Read-only; spawnables are listed by get_sequence_spawnables, and every binding with its tracks by get_sequence_bindings.
//   @param sequence_name - Asset name (not a path) of the Level Sequence, e.g. "LS_Intro"; the first match under /Game is used, ignoring case.
//   @returns { sequence_name: string, possessables: [{guid, name, class, parent_guid}], possessable_count: int }

TSharedPtr<FJsonObject> HandleGetSequencePossessables(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }
    const UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(LevelSequenceUtils::FindByName(SequenceName), SequenceName, MovieScene)) { return Failure; }

    TArray<TSharedPtr<FJsonValue>> PossArray;
    const int32 Count = MovieScene->GetPossessableCount();
    for (int32 i = 0; i < Count; ++i)
    {
        FMovieScenePossessable& Poss = const_cast<UMovieScene*>(MovieScene)->GetPossessable(i);
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("guid"), Poss.GetGuid().ToString());
        Obj->SetStringField(TEXT("name"), Poss.GetName());
        const UClass* Cls = Poss.GetPossessedObjectClass();
        Obj->SetStringField(TEXT("class"), Cls ? Cls->GetName() : TEXT("null"));
        const FGuid& ParentGuid = Poss.GetParent();
        Obj->SetStringField(TEXT("parent_guid"), ParentGuid.IsValid() ? ParentGuid.ToString() : TEXT(""));
        PossArray.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetArrayField(TEXT("possessables"), PossArray);
    Response->SetNumberField(TEXT("possessable_count"), Count);
    return mcp_common::MakeOkReply(Response);
}

// MCP:remove_sequence_binding - Remove a spawnable or possessable binding, found by GUID, from a Level Sequence named by asset name under /Game, then save the sequence; a failed save returns success false. Fails when the GUID is not a spawnable or possessable of the sequence. Returns removed_as; get GUIDs from get_sequence_bindings.
//   @param sequence_name - Asset name (not a path) of the Level Sequence, e.g. "LS_Intro"; the first match under /Game is used, ignoring case.
//   @param binding_guid - GUID of the binding to remove, as get_sequence_bindings returns it.
//   @returns { sequence_name: string, binding_guid: string, removed: bool, removed_as: string, saved: bool }

TSharedPtr<FJsonObject> HandleRemoveSequenceBinding(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName, GuidStr;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequenceAndBinding(Params, SequenceName, GuidStr)) { return Missing; }

    FGuid BindingGuid;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentSequencerImpl::ParseGuid(GuidStr, TEXT("binding_guid"), BindingGuid)) { return Invalid; }

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    const FScopedTransaction Transaction(LOCTEXT("RemoveSequenceBinding", "MCP: Remove Sequence Binding"));
    MovieScene->Modify();

    // Try removing as spawnable first, then as possessable
    bool bRemoved = false;
    FString RemovedAs;
    if (MovieScene->FindSpawnable(BindingGuid))
    {
        bRemoved = MovieScene->RemoveSpawnable(BindingGuid);
        RemovedAs = TEXT("spawnable");
    }
    else if (MovieScene->FindPossessable(BindingGuid))
    {
        bRemoved = MovieScene->RemovePossessable(BindingGuid);
        RemovedAs = TEXT("possessable");
    }
    else
    {
        // RemoveBinding is protected in UMovieScene -- cannot call from outside.
        // Return honest error if neither spawnable nor possessable matched.
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Binding '%s' is neither a spawnable nor a possessable; cannot remove"), *GuidStr));
    }

    if (!bRemoved)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to remove binding '%s'"), *GuidStr));

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("binding_guid"), GuidStr);
    Response->SetStringField(TEXT("removed_as"), RemovedAs);
    Response->SetBoolField(TEXT("removed"), bRemoved);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("remove_sequence_binding"));
}

// MCP:get_binding_tracks - List all tracks for a given binding GUID with class, display name, section count.
//   @param sequence_name - Name of the level sequence asset.
//   @param binding_guid - GUID string of the binding.
//   @returns { sequence_name: string, binding_guid: string, tracks: [{class, display_name, section_count, is_empty}], track_count: int }

TSharedPtr<FJsonObject> HandleGetBindingTracks(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName, GuidStr;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequenceAndBinding(Params, SequenceName, GuidStr)) { return Missing; }

    FGuid BindingGuid;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentSequencerImpl::ParseGuid(GuidStr, TEXT("binding_guid"), BindingGuid)) { return Invalid; }

    const UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(LevelSequenceUtils::FindByName(SequenceName), SequenceName, MovieScene)) { return Failure; }

    const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid);
    if (!Binding)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Binding '%s' not found in sequence"), *GuidStr));

    TArray<TSharedPtr<FJsonValue>> TracksArray;
    for (const UMovieSceneTrack* Track : Binding->GetTracks())
    {
        if (!Track) { continue; }
        TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
        TrackObj->SetStringField(TEXT("class"), Track->GetClass()->GetName());
        TrackObj->SetStringField(TEXT("display_name"), Track->GetDisplayName().ToString());
        TrackObj->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
        TrackObj->SetBoolField(TEXT("is_empty"), Track->IsEmpty());
        TracksArray.Add(MakeShared<FJsonValueObject>(TrackObj));
    }

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("binding_guid"), GuidStr);
    Response->SetArrayField(TEXT("tracks"), TracksArray);
    Response->SetNumberField(TEXT("track_count"), TracksArray.Num());
    return mcp_common::MakeOkReply(Response);
}

// ===========================================================================
// Advanced Sequencer authoring (asset-level only)
//
// These commands are real, headless, and operate on the ULevelSequence /
// UMovieScene asset model only. They do NOT touch the runtime player
// (ULevelSequencePlayer::PlayRate is PIE-only -- we explicitly do NOT expose
// it; the asset-level "play rate" surface is UMovieScene::SetDisplayRate,
// which we do expose).
//
//   1. set_section_pre_post_rollout -- UMovieSceneSection::SetPreRollFrames
//                                      / SetPostRollFrames (inline, FFrameNumber)
//   2. set_sequence_view_range      -- UMovieScene::SetViewRange(double, double)
//   3. set_sequence_working_range   -- UMovieScene::SetWorkingRange(double, double)
//   4. set_sequence_display_rate    -- UMovieScene::SetDisplayRate(FFrameRate)
//   5. get_sequence_master_tracks   -- enumerate MovieScene->GetTracks(), split by
//                                      Track->FindObjectBindingGuid() (invalid =
//                                      master track, valid = binding track).
//
// Runtime playback lives in the *_level_sequence_runtime commands
// (play_level_sequence_runtime and its siblings), which need a PIE or game world.
// ===========================================================================

namespace UEAgentSequencerImpl
{
    // Reads the required start_seconds and end_seconds; the error reply when either is missing or the range is
    // empty.
    TSharedPtr<FJsonObject> ReadSecondsRange(const TSharedPtr<FJsonObject>& Params, double& OutStart, double& OutEnd)
    {
        if (!Params->TryGetNumberField(TEXT("start_seconds"), OutStart) ||
            !Params->TryGetNumberField(TEXT("end_seconds"), OutEnd))
        {
            return mcp_common::MakeErrorReply(
                TEXT("Missing required parameters: 'start_seconds' and 'end_seconds'"));
        }
        if (OutEnd <= OutStart)
        {
            return mcp_common::MakeErrorReply(
                TEXT("'end_seconds' must be greater than 'start_seconds'"));
        }
        return nullptr;
    }

    // A reply payload with the sequence's sequence_path and the Stored editor range (0 for an open bound), verified
    // against the requested one. The view/working range getters live on FMovieSceneEditorData, not on UMovieScene.
    TSharedPtr<FJsonObject> NewSecondsRangePayload(const ULevelSequence* Sequence, const TRange<double>& Stored,
        double RequestedStart, double RequestedEnd)
    {
        const double StoredStart = Stored.GetLowerBound().IsClosed() ? Stored.GetLowerBoundValue() : 0.0;
        const double StoredEnd = Stored.GetUpperBound().IsClosed() ? Stored.GetUpperBoundValue() : 0.0;

        TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
        Response->SetNumberField(TEXT("start_seconds"), StoredStart);
        Response->SetNumberField(TEXT("end_seconds"), StoredEnd);
        Response->SetBoolField(TEXT("verified"),
            FMath::IsNearlyEqual(StoredStart, RequestedStart) && FMath::IsNearlyEqual(StoredEnd, RequestedEnd));
        return Response;
    }
}

// MCP:set_section_pre_post_rollout - Set pre-roll and/or post-roll frames on a sequencer section.  Pre-roll = frames the section evaluates BEFORE its visible range. Post-roll = frames the section evaluates AFTER its visible range. Both are stored as FFrameNumber on UMovieSceneSection.  Pass at least one of pre_roll_frames or post_roll_frames (a non-None value). A 0 value means "no roll" (clears the roll window). The values are written via the inline SetPreRollFrames / SetPostRollFrames setters and read back through the section's public FFrameNumber fields.
//   @param sequence_path - Asset path of the Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param binding_guid - GUID string of the binding the section lives under.
//   @param track_type - Optional class filter ("transform"|"audio"|"event"|"fade"|etc.). Default "transform".
//   @param track_index - Index into the binding's tracks (0 = first track, default 0; used when track_type is omitted).
//   @param section_index - Index of the section on the resolved track (default 0).
//   @param pre_roll_frames - Optional pre-roll frames (omit to leave unchanged).
//   @param post_roll_frames - Optional post-roll frames (omit to leave unchanged).
//   @param binding_name_or_guid - Alias for binding_guid, read only when binding_guid is absent; it must also be a GUID string, a bound object's name is not resolved.
//   @returns { sequence_path: string, pre_roll_frames: int, post_roll_frames: int, set: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetSectionPrePostRollout(const TSharedPtr<FJsonObject>& Params)
{
    ULevelSequence* Sequence = nullptr;
    UMovieSceneSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ResolveSectionFromParams(Params, Sequence, Section)) { return Failure; }

    // Both params are optional; only set what the caller passed.
    bool bAnySet = false;
    double PreRoll = 0.0, PostRoll = 0.0;
    if (Params->TryGetNumberField(TEXT("pre_roll_frames"), PreRoll))
    {
        Section->Modify();
        Section->SetPreRollFrames(FMath::RoundToInt(PreRoll));
        bAnySet = true;
    }
    if (Params->TryGetNumberField(TEXT("post_roll_frames"), PostRoll))
    {
        Section->Modify();
        Section->SetPostRollFrames(FMath::RoundToInt(PostRoll));
        bAnySet = true;
    }
    if (!bAnySet)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing at least one of 'pre_roll_frames' or 'post_roll_frames'"));
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetNumberField(TEXT("pre_roll_frames"), Section->GetPreRollFrames());
    Response->SetNumberField(TEXT("post_roll_frames"), Section->GetPostRollFrames());
    Response->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_section_pre_post_rollout"));
}

// MCP:set_sequence_view_range - Set the editor view range of a Level Sequence (in seconds).  The view range controls the visible timeline window in the Sequencer editor (does NOT affect playback or asset playback range). Backed by UMovieScene::SetViewRange(double, double) -- MOVIESCENE_API.
//   @param sequence_path - Asset path of the Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param start_seconds - Start of the view range, in seconds.
//   @param end_seconds - End of the view range, in seconds. Must be > start_seconds.
//   @returns { sequence_path: string, start_seconds: float, end_seconds: float, verified: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetSequenceViewRange(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequencePath(Params, SequencePath)) { return Missing; }
    double StartSec = 0.0, EndSec = 0.0;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentSequencerImpl::ReadSecondsRange(Params, StartSec, EndSec)) { return Invalid; }

    ULevelSequence* Sequence = LoadSequenceByPathOrName(SequencePath);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequencePath, MovieScene)) { return Failure; }

    const FScopedTransaction Transaction(LOCTEXT("SetSequenceViewRange", "MCP: Set Sequence View Range"));
    MovieScene->Modify();
    MovieScene->SetViewRange(StartSec, EndSec);
    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = UEAgentSequencerImpl::NewSecondsRangePayload(
        Sequence, MovieScene->GetEditorData().GetViewRange(), StartSec, EndSec);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_sequence_view_range"));
}

// MCP:set_sequence_working_range - Sets the working range of a Level Sequence (path or bare asset name): the scrollable bounds of the Sequencer timeline, in seconds. Editor UI state only, it changes neither playback nor evaluation. The engine keeps it in a transient field, so although the package is saved the value is not written: after a reload the working range is recomputed from the playback range. verified compares the stored range with the request. For the visible window use set_sequence_view_range; for what plays use set_sequence_playback_range.
//   @param sequence_path - Asset path of the Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param start_seconds - Start of the working range, in seconds.
//   @param end_seconds - End of the working range, in seconds. Must be > start_seconds.
//   @returns { sequence_path: string, start_seconds: float, end_seconds: float, verified: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetSequenceWorkingRange(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequencePath(Params, SequencePath)) { return Missing; }
    double StartSec = 0.0, EndSec = 0.0;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentSequencerImpl::ReadSecondsRange(Params, StartSec, EndSec)) { return Invalid; }

    ULevelSequence* Sequence = LoadSequenceByPathOrName(SequencePath);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequencePath, MovieScene)) { return Failure; }

    const FScopedTransaction Transaction(LOCTEXT("SetSequenceWorkingRange", "MCP: Set Sequence Working Range"));
    MovieScene->Modify();
    MovieScene->SetWorkingRange(StartSec, EndSec);
    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = UEAgentSequencerImpl::NewSecondsRangePayload(
        Sequence, MovieScene->GetEditorData().GetWorkingRange(), StartSec, EndSec);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_sequence_working_range"));
}

// MCP:set_sequence_display_rate - Set the display frame rate of a Level Sequence.  The display rate is the asset-level "play rate" (frames per second of the timeline). Changing it rescales how existing frame numbers map to time. Backed by UMovieScene::SetDisplayRate(FFrameRate).  Note: The numerator/denominator are integers (FFrameRate), so the fps argument is rounded to the nearest whole-number fps. For non-integer rates (e.g. 23.976) the caller should still pass the closest whole number; the engine does not support fractional FFrameRate numerators without a custom non-integer tick resolution.
//   @param sequence_path - Asset path of the Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param fps - New display rate in frames per second. Must be > 0.
//   @returns { sequence_path: string, fps: float, numerator: int, denominator: int, verified: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetSequenceDisplayRate(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequencePath(Params, SequencePath)) { return Missing; }
    double Fps = 0.0;
    if (!Params->TryGetNumberField(TEXT("fps"), Fps) || Fps <= 0.0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing or invalid 'fps' parameter (must be > 0)"));
    }

    ULevelSequence* Sequence = LoadSequenceByPathOrName(SequencePath);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequencePath, MovieScene)) { return Failure; }

    // Snap to whole-number numerator/denominator (FFrameRate requires integers).
    const int32 Num = FMath::RoundToInt(Fps);
    const int32 Den = 1;
    const FFrameRate NewRate(Num, Den);

    const FScopedTransaction Transaction(LOCTEXT("SetSequenceDisplayRate", "MCP: Set Sequence Display Rate"));
    MovieScene->Modify();
    MovieScene->SetDisplayRate(NewRate);
    const bool bSaved = SaveSequencePackage(Sequence);

    const FFrameRate ReadBack = MovieScene->GetDisplayRate();
    const double ReadBackFps = Den > 0 ? static_cast<double>(ReadBack.Numerator) / static_cast<double>(ReadBack.Denominator) : 0.0;

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetNumberField(TEXT("fps"), ReadBackFps);
    Response->SetNumberField(TEXT("numerator"), ReadBack.Numerator);
    Response->SetNumberField(TEXT("denominator"), ReadBack.Denominator);
    Response->SetBoolField(TEXT("verified"), FMath::IsNearlyEqual(ReadBackFps, Fps, 0.01));
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_sequence_display_rate"));
}

// MCP:get_sequence_master_tracks - List the master tracks and binding tracks of a Level Sequence.  A track is a "master track" if it has no binding GUID (applies to the sequence globally, e.g. a FadeTrack, AudioTrack, EventTrack). Binding tracks have a non-empty FGuid. This command returns both lists with class, name, section_count, binding_guid fields.
//   @param sequence_path - Asset path of the Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @returns { sequence_path: string, total_track_count: int, master_track_count: int, binding_track_count: int, master_tracks: [{class, name, section_count, binding_guid}], binding_tracks: [{class, name, section_count, binding_guid}] }

TSharedPtr<FJsonObject> HandleGetSequenceMasterTracks(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequencePath(Params, SequencePath)) { return Missing; }

    ULevelSequence* Sequence = LoadSequenceByPathOrName(SequencePath);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequencePath, MovieScene)) { return Failure; }

    // A track is a "master track" if FindBindingFromTrack returns nullptr (no
    // binding GUID). Binding tracks have a non-null FMovieSceneBinding.
    TArray<TSharedPtr<FJsonValue>> MasterTracks;
    TArray<TSharedPtr<FJsonValue>> BindingTracks;
    int32 NumMaster = 0, NumBinding = 0;
    for (UMovieSceneTrack* Track : MovieScene->GetTracks())
    {
        if (!Track)
        {
            continue;
        }
        TSharedPtr<FJsonObject> TObj = MakeShared<FJsonObject>();
        const FString ClassName = Track->GetClass()->GetName();
        const FString TrackName = Track->GetTrackName().IsNone() ? FString() : Track->GetTrackName().ToString();
        TObj->SetStringField(TEXT("class"), ClassName);
        TObj->SetStringField(TEXT("name"), TrackName);
        TObj->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
        // UMovieSceneTrack::FindObjectBindingGuid() (MOVIESCENE_API, MovieSceneTrack.h)
        // returns the binding FGuid -- an invalid FGuid
        // means the track is a master track (no binding). Cast-checked.
        const FGuid BindingGuid = Track->FindObjectBindingGuid();
        if (BindingGuid.IsValid())
        {
            TObj->SetStringField(TEXT("binding_guid"), BindingGuid.ToString());
            BindingTracks.Add(MakeShared<FJsonValueObject>(TObj));
            ++NumBinding;
        }
        else
        {
            TObj->SetStringField(TEXT("binding_guid"), FString());
            MasterTracks.Add(MakeShared<FJsonValueObject>(TObj));
            ++NumMaster;
        }
    }

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetNumberField(TEXT("total_track_count"), NumMaster + NumBinding);
    Response->SetNumberField(TEXT("master_track_count"), NumMaster);
    Response->SetNumberField(TEXT("binding_track_count"), NumBinding);
    Response->SetArrayField(TEXT("master_tracks"), MasterTracks);
    Response->SetArrayField(TEXT("binding_tracks"), BindingTracks);
    return mcp_common::MakeOkReply(Response);
}

// ========================================================================== //
// Sequencer takes detail
//
// Per-section / per-track authoring:
//
//   set_transform_track_interp_mode  set a section's EMovieSceneBlendType
//                                   (absolute|relative|additive|
//                                   additive_from_base|override). The
//                                   engine exposes this as "Blend Type" in
//                                   the UI; for transform tracks it controls
//                                   how the section contributes to the
//                                   actor's transform when layered with other
//                                   sections (additive = delta from base;
//                                   override = full replace, etc.).
//   add_camera_focus_track           add a UMovieSceneFloatTrack on
//                                   ACineCameraActor::ManualFocusDistance.
//                                   Equivalent to add_property_track with
//                                   property_type=float + property_name=
//                                   ManualFocusDistance, but with a
//                                   domain-specific name + sane defaults.
//   add_visibility_track             add a UMovieSceneVisibilityTrack on the
//                                   binding (wraps add_property_track with
//                                   visibility + property_name=bVisible).
//   add_weighted_value_track         add a UMovieSceneFloatTrack on a numeric
//                                   property of the caller's choice
//                                   (weight, alpha, scalar). Provides a
//                                   named helper for the common case "I
//                                   want a float track on a property named X".
//   set_property_track_keyframe_count
//                                   replace a property track's section keys
//                                   with exactly N keys (evenly spaced
//                                   from the section start, default value
//                                   repeated). Useful to bulk-reseed a track
//                                   when re-targeting.
//   duplicate_keyframe               clone an existing key (Float / Vector /
//                                   Color / Bool channel) to a new time
//                                   within the same section, preserving the
//                                   original value. The original key stays
//                                   in place (this is a copy, not a move).
//
// set_transform_track_interp_mode, set_property_track_keyframe_count and
// duplicate_keyframe follow the section pattern: section guard + scoped
// transaction + Section->Modify() + SaveSequencePackage(). The add_*_track
// commands delegate to add_property_track.
// ========================================================================== //

// MCP:set_transform_track_interp_mode - Sets the blend type of one section on a Level Sequence binding (how it layers with other sections, e.g. additive adds a delta, override replaces), in an undo transaction, and saves the sequence. Despite the name it changes no key interpolation and accepts any track kind. The section is section_index on the binding's first track of track_type, else its track_index-th track; a blend type the track does not support is refused. set_section_blend_type does the same without that check; for slerp rotation use set_transform_section_quaternion_interp.
//   @param sequence_path - Asset path of the Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param binding_guid - GUID of the binding the section lives under.
//   @param track_type - Optional track kind such as "transform", "float" or "audio": picks the binding's first track of that kind. Omitted or unrecognised, track_index picks the track instead.
//   @param track_index - Index into the binding's tracks (0 = first track, default 0).
//   @param section_index - Index of the section on the resolved track (default 0).
//   @param interp_mode - Blend type string; one of absolute|additive|relative|additive_from_base|override. Also accepted under "blend_type".
//   @param blend_type - Alias for interp_mode (absolute|additive|relative|additive_from_base|override)
//   @param binding_name_or_guid - Alias for binding_guid, read only when binding_guid is absent; it must also be a GUID string, a bound object's name is not resolved.
//   @returns { sequence_path: string, interp_mode: string, blend_type: string, blend_type_valid: bool, applied: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetTransformTrackInterpMode(const TSharedPtr<FJsonObject>& Params)
{
    ULevelSequence* Sequence = nullptr;
    UMovieSceneSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ResolveSectionFromParams(Params, Sequence, Section)) { return Failure; }

    // Aliases: this command's name is "interp_mode"; the underlying API
    // is called "blend_type". We accept either so the two names
    // read consistently. Default: keep current.
    FString ModeStr;
    if (!Params->TryGetStringField(TEXT("interp_mode"), ModeStr) || ModeStr.IsEmpty())
    {
        Params->TryGetStringField(TEXT("blend_type"), ModeStr);
    }
    if (ModeStr.IsEmpty())
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: interp_mode (or blend_type)"));
    }

    EMovieSceneBlendType NewBlend;
    if (!StringToBlendType(ModeStr, NewBlend))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid interp_mode '%s' (use absolute|additive|relative|additive_from_base|override)"),
                            *ModeStr));
    }

    // Verify the track supports the requested blend. Most property tracks
    // support all 5 types; some (e.g. Visibility) reject Override.
    UMovieSceneTrack* OwnerTrack = Section->GetTypedOuter<UMovieSceneTrack>();
    if (OwnerTrack)
    {
        const FMovieSceneBlendTypeField& Supported = OwnerTrack->GetSupportedBlendTypes();
        if (!Supported.Contains(NewBlend))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Track '%s' does not support blend_type '%s'"),
                                *OwnerTrack->GetClass()->GetName(), *ModeStr));
        }
    }

    const FScopedTransaction Transaction(LOCTEXT("SetTransformTrackInterpMode", "MCP: Set Transform Track Interp Mode"));
    Section->Modify();
    Section->SetBlendType(NewBlend);

    const bool bSaved = SaveSequencePackage(Sequence);
    const FOptionalMovieSceneBlendType OptBlend = Section->GetBlendType();

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetStringField(TEXT("interp_mode"), ModeStr);
    Response->SetStringField(TEXT("blend_type"), BlendTypeToString(OptBlend.BlendType));
    Response->SetBoolField(TEXT("blend_type_valid"), OptBlend.bIsValid);
    Response->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_transform_track_interp_mode"));
}

namespace UEAgentSequencerImpl
{
    // The add_property_track request for one property, carrying the caller's optional track_label.
    TSharedPtr<FJsonObject> MakePropertyTrackRequest(const TSharedPtr<FJsonObject>& Params, const FString& SequenceName,
        const FString& BindingGuid, const FString& PropertyType, const FString& PropertyName)
    {
        TSharedPtr<FJsonObject> Inner = MakeShared<FJsonObject>();
        Inner->SetStringField(TEXT("sequence_name"), SequenceName);
        Inner->SetStringField(TEXT("binding_guid"), BindingGuid);
        Inner->SetStringField(TEXT("property_type"), PropertyType);
        Inner->SetStringField(TEXT("property_name"), PropertyName);
        if (Params->HasField(TEXT("track_label")))
        {
            Inner->SetStringField(TEXT("track_label"), Params->GetStringField(TEXT("track_label")));
        }
        return Inner;
    }
}

// MCP:add_camera_focus_track - Add a Float property track on ACineCameraActor::ManualFocusDistance to a Level Sequence binding.  Thin domain wrapper over add_property_track.
//   @param sequence_name - Name of the target Level Sequence.
//   @param binding_guid - GUID of the camera binding to attach the focus track to.
//   @param track_label - Optional display label for the new track.
//   @returns Forwarded response from add_property_track: { sequence_name, binding_guid, property_type, property_name, track_class, section_class, section_created, section_count, saved }
TSharedPtr<FJsonObject> HandleAddCameraFocusTrack(const TSharedPtr<FJsonObject>& Params)
{
    // Thin domain helper: add a float track on ACineCameraActor::ManualFocusDistance.
    // The user's call site stays intent-revealing (camera focus) without
    // having to know the underlying property path or float-track class.
    FString SequenceName, GuidStr;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequenceAndBinding(Params, SequenceName, GuidStr)) { return Missing; }

    // Mirror the params expected by HandleAddPropertyTrack: property_type +
    // property_name. Override the defaults for the camera-focus intent.
    return HandleAddPropertyTrack(UEAgentSequencerImpl::MakePropertyTrackRequest(
        Params, SequenceName, GuidStr, TEXT("float"), TEXT("ManualFocusDistance")));
}

// MCP:add_visibility_track - Add a Visibility property track on bVisible to a Level Sequence binding.  Thin domain wrapper over add_property_track.
//   @param sequence_name - Name of the target Level Sequence.
//   @param binding_guid - GUID of the binding to attach the visibility track to.
//   @param track_label - Optional display label for the new track.
//   @returns Forwarded response from add_property_track: { sequence_name, binding_guid, property_type, property_name, track_class, section_class, section_created, section_count, saved }
TSharedPtr<FJsonObject> HandleAddVisibilityTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName, GuidStr;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequenceAndBinding(Params, SequenceName, GuidStr)) { return Missing; }

    return HandleAddPropertyTrack(UEAgentSequencerImpl::MakePropertyTrackRequest(
        Params, SequenceName, GuidStr, TEXT("visibility"), TEXT("bVisible")));
}

// MCP:add_weighted_value_track - Add a Float property track on a numeric property (weight / alpha / scalar blend) to a Level Sequence binding.  Thin domain wrapper over add_property_track.
//   @param sequence_name - Name of the target Level Sequence.
//   @param binding_guid - GUID of the binding to attach the weighted value track to.
//   @param property_name - Name of the float property to drive (default "Weight").
//   @param track_label - Optional display label for the new track.
//   @returns Forwarded response from add_property_track: { sequence_name, binding_guid, property_type, property_name, track_class, section_class, section_created, section_count, saved }
TSharedPtr<FJsonObject> HandleAddWeightedValueTrack(const TSharedPtr<FJsonObject>& Params)
{
    // "weighted value" == a UMovieSceneFloatTrack on a scalar property
    // (alpha / weight / scalar blend). The caller passes property_name (the
    // float field to drive); property_type stays "float". There is no
    // dedicated UMovieSceneWeightTrack class, so the float track is
    // the canonical surface.
    FString SequenceName, GuidStr, PropertyName;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequenceAndBinding(Params, SequenceName, GuidStr)) { return Missing; }
    // Default to a generic "Weight" name so callers can introspect the
    // result; explicit property_name overrides.
    if (!Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
    {
        PropertyName = TEXT("Weight");
    }

    return HandleAddPropertyTrack(UEAgentSequencerImpl::MakePropertyTrackRequest(
        Params, SequenceName, GuidStr, TEXT("float"), PropertyName));
}

// MCP:set_property_track_keyframe_count - Resize a property track's first section to exactly N keys evenly spaced from the section start, all carrying the same default value.
//   @param sequence_name - Name of the target Level Sequence.
//   @param binding_guid - GUID of the binding.
//   @param property_type - One of "Float", "Bool", "Vector", "Color", "Visibility".
//   @param key_count - Target number of keys (>= 0). 0 clears all keys; 1 seeds at section start; >=2 evenly distributes.
//   @param default_value - Value to use for every resampled key (default 0).
//   @returns { sequence_name: string, binding_guid: string, property_type: string, key_count: int, default_value: float, keys_written: int, applied: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetPropertyTrackKeyframeCount(const TSharedPtr<FJsonObject>& Params)
{
    // Resize a property track's first section to exactly N keys, evenly
    // spaced from the section start, all carrying the same default value.
    // Useful when re-targeting: e.g. bake 60 sampled frames into a track to
    // match a 60-fps timeline without re-keying each by hand.
    FString SequenceName, GuidStr, PropertyTypeRaw;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequenceAndBinding(Params, SequenceName, GuidStr)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property_type"), PropertyTypeRaw)) { return Missing; }
    const FString PropertyType = NormalizePropertyType(PropertyTypeRaw);

    int32 KeyCount = 0;
    if (!Params->TryGetNumberField(TEXT("key_count"), KeyCount) || KeyCount < 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: key_count (>= 0)"));
    }

    const double DefaultValue = Params->HasField(TEXT("default_value"))
        ? Params->GetNumberField(TEXT("default_value"))
        : 0.0;

    FGuid BindingGuid;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentSequencerImpl::ParseGuid(GuidStr, TEXT("binding_guid"), BindingGuid)) { return Invalid; }

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    UMovieSceneSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::FindPropertySection(MovieScene, BindingGuid, PropertyType, PropertyTypeRaw, Section))
    {
        return Failure;
    }

    const FScopedTransaction Transaction(LOCTEXT("SetPropertyTrackKeyframeCount", "MCP: Set Property Track Keyframe Count"));
    Section->Modify();

    // Sample times evenly across the section. For KeyCount == 0 we keep the
    // section but clear all keys; for KeyCount == 1 we seed the section
    // start; >=2 evenly distribute.
    const TRange<FFrameNumber> Range = Section->GetRange();
    const FFrameNumber StartFrame = Range.GetLowerBound().IsClosed()
        ? Range.GetLowerBoundValue()
        : FFrameNumber(0);
    const FFrameNumber EndFrame = Range.GetUpperBound().IsClosed()
        ? Range.GetUpperBoundValue()
        : StartFrame + FFrameNumber(1);

    const int64 Span = FMath::Max<int64>(1, (EndFrame - StartFrame).Value);

    int32 KeysWritten = 0;
    const FFrameRate TickResolution = MovieScene->GetTickResolution();
    if (PropertyType == TEXT("bool") || PropertyType == TEXT("visibility"))
    {
        const bool bValue = (DefaultValue != 0.0);
        TArrayView<FMovieSceneBoolChannel*> BoolChannels =
            Section->GetChannelProxy().GetChannels<FMovieSceneBoolChannel>();
        for (FMovieSceneBoolChannel* Channel : BoolChannels)
        {
            if (!Channel) { continue; }
            Channel->GetData().Reset();
            for (int32 i = 0; i < KeyCount; ++i)
            {
                const FFrameNumber Frame(StartFrame.Value + static_cast<int32>(KeyCount > 1 ? (Span * i) / (KeyCount - 1) : 0));
                Channel->GetData().UpdateOrAddKey(Frame, bValue);
                ++KeysWritten;
            }
        }
    }
    else if (PropertyType == TEXT("float") || PropertyType == TEXT("vector") || PropertyType == TEXT("color"))
    {
        // Float tracks have one FMovieSceneFloatChannel and Vector / Color tracks one per
        // component (X/Y/Z or R/G/B); the same default is distributed across all of them.
        TArrayView<FMovieSceneFloatChannel*> FloatChannels =
            Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
        for (FMovieSceneFloatChannel* Channel : FloatChannels)
        {
            if (!Channel) { continue; }
            Channel->GetData().Reset();
            for (int32 i = 0; i < KeyCount; ++i)
            {
                const FFrameNumber Frame(StartFrame.Value + static_cast<int32>(KeyCount > 1 ? (Span * i) / (KeyCount - 1) : 0));
                Channel->GetData().UpdateOrAddKey(Frame, FMovieSceneFloatValue(static_cast<float>(DefaultValue)));
                ++KeysWritten;
            }
        }
    }
    else
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unsupported property_type '%s' for key_count resize"), *PropertyTypeRaw));
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("binding_guid"), GuidStr);
    Response->SetStringField(TEXT("property_type"), PropertyType);
    Response->SetNumberField(TEXT("key_count"), KeyCount);
    Response->SetNumberField(TEXT("default_value"), DefaultValue);
    Response->SetNumberField(TEXT("keys_written"), KeysWritten);
    Response->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_property_track_keyframe_count"));
}

// MCP:duplicate_keyframe - Clone an existing key (Float / Vector / Color / Bool channel) from source_time_seconds to dest_time_seconds, preserving the original value.
//   @param sequence_name - Name of the target Level Sequence.
//   @param binding_guid - GUID of the binding owning the property track.
//   @param property_type - One of "Float", "Bool", "Vector", "Color", "Visibility".
//   @param source_time_seconds - Time (seconds) of the source key to copy.
//   @param dest_time_seconds - Destination time (seconds) for the cloned key.
//   @param channel_index - Index of the channel to read/write (default 0).
//   @returns { sequence_name: string, binding_guid: string, property_type: string, source_time_seconds: float, dest_time_seconds: float, source_frame: int, dest_frame: int, channel_index: int, duplicated_value: string, keys_written: int, applied: bool, saved: bool }
TSharedPtr<FJsonObject> HandleDuplicateKeyframe(const TSharedPtr<FJsonObject>& Params)
{
    // Clone an existing key (Float / Vector / Color / Bool channel) from
    // source_time_seconds to dest_time_seconds, preserving the original
    // value. The source key stays in place; the destination gets a new key
    // (or replaces an existing one if dest_time already has a key).
    FString SequenceName, GuidStr, PropertyTypeRaw;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequenceAndBinding(Params, SequenceName, GuidStr)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property_type"), PropertyTypeRaw)) { return Missing; }
    const FString PropertyType = NormalizePropertyType(PropertyTypeRaw);

    if (!Params->HasField(TEXT("source_time_seconds")) || !Params->HasField(TEXT("dest_time_seconds")))
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: source_time_seconds + dest_time_seconds"));
    }
    const double SourceTime = Params->GetNumberField(TEXT("source_time_seconds"));
    const double DestTime = Params->GetNumberField(TEXT("dest_time_seconds"));

    int32 ChannelIndex = 0;
    if (Params->HasField(TEXT("channel_index")))
    {
        ChannelIndex = static_cast<int32>(Params->GetNumberField(TEXT("channel_index")));
    }

    FGuid BindingGuid;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentSequencerImpl::ParseGuid(GuidStr, TEXT("binding_guid"), BindingGuid)) { return Invalid; }

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    UMovieScenePropertyTrack* PropTrack = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::FindPropertyTrack(MovieScene, BindingGuid, PropertyType, PropertyTypeRaw, PropTrack))
    {
        return Failure;
    }
    if (!PropTrack || PropTrack->GetAllSections().Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("No property track / section on this binding"));
    }
    UMovieSceneSection* Section = PropTrack->GetAllSections()[0];

    const FScopedTransaction Transaction(LOCTEXT("DuplicateKeyframe", "MCP: Duplicate Keyframe"));
    Section->Modify();

    const FFrameRate TickResolution = MovieScene->GetTickResolution();
    const FFrameNumber SourceFrame = TickResolution.AsFrameTime(SourceTime).RoundToFrame();
    const FFrameNumber DestFrame = TickResolution.AsFrameTime(DestTime).RoundToFrame();

    int32 KeysWritten = 0;
    FString ValueEcho;
    if (PropertyType == TEXT("bool") || PropertyType == TEXT("visibility"))
    {
        TArrayView<FMovieSceneBoolChannel*> Channels = Section->GetChannelProxy().GetChannels<FMovieSceneBoolChannel>();
        if (!Channels.IsValidIndex(ChannelIndex))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("channel_index %d out of range (track has %d bool channels)"),
                                ChannelIndex, Channels.Num()));
        }
        FMovieSceneBoolChannel* Channel = Channels[ChannelIndex];
        if (!Channel)
        {
            return mcp_common::MakeErrorReply(TEXT("Bool channel is null"));
        }
        // Find the source key by frame and copy its value to DestFrame. Bool
        // channels store raw bool values via TArrayView<const bool>, with
        // times mirrored at TArrayView<const FFrameNumber>.
        TArrayView<const bool> SourceValues = Channel->GetData().GetValues();
        TArrayView<const FFrameNumber> SourceTimes = Channel->GetData().GetTimes();
        int32 SourceIndex = INDEX_NONE;
        for (int32 i = 0; i < SourceTimes.Num(); ++i)
        {
            if (SourceTimes[i] == SourceFrame) { SourceIndex = i; break; }
        }
        if (SourceIndex == INDEX_NONE)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("source_time_seconds=%g (frame %d) not found on the bool channel"),
                                SourceTime, SourceFrame.Value));
        }
        const bool bValue = SourceValues[SourceIndex];
        Channel->GetData().UpdateOrAddKey(DestFrame, bValue);
        ValueEcho = bValue ? TEXT("true") : TEXT("false");
        ++KeysWritten;
    }
    else
    {
        // Float / Vector / Color all back onto FMovieSceneFloatChannel.
        TArrayView<FMovieSceneFloatChannel*> Channels = Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
        if (!Channels.IsValidIndex(ChannelIndex))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("channel_index %d out of range (track has %d float channels)"),
                                ChannelIndex, Channels.Num()));
        }
        FMovieSceneFloatChannel* Channel = Channels[ChannelIndex];
        if (!Channel)
        {
            return mcp_common::MakeErrorReply(TEXT("Float channel is null"));
        }
        // Float channels: GetValues() returns TArrayView<FMovieSceneFloatValue>
        // (each entry carries the value + the tangent/interp metadata).
        TArrayView<const FMovieSceneFloatValue> FloatValues = Channel->GetData().GetValues();
        TArrayView<const FFrameNumber> FloatTimes = Channel->GetData().GetTimes();
        int32 SourceIndex = INDEX_NONE;
        for (int32 i = 0; i < FloatTimes.Num(); ++i)
        {
            if (FloatTimes[i] == SourceFrame) { SourceIndex = i; break; }
        }
        if (SourceIndex == INDEX_NONE)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("source_time_seconds=%g (frame %d) not found on the float channel"),
                                SourceTime, SourceFrame.Value));
        }
        const float Value = FloatValues[SourceIndex].Value;
        // FMovieSceneFloatValue's single-arg ctor is explicit; construct it
        // explicitly so the call doesn't try to silently implicit-convert.
        Channel->GetData().UpdateOrAddKey(DestFrame, FMovieSceneFloatValue(Value));
        ValueEcho = FString::Printf(TEXT("%g"), Value);
        ++KeysWritten;
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("binding_guid"), GuidStr);
    Response->SetStringField(TEXT("property_type"), PropertyType);
    Response->SetNumberField(TEXT("source_time_seconds"), SourceTime);
    Response->SetNumberField(TEXT("dest_time_seconds"), DestTime);
    Response->SetNumberField(TEXT("source_frame"), SourceFrame.Value);
    Response->SetNumberField(TEXT("dest_frame"), DestFrame.Value);
    Response->SetNumberField(TEXT("channel_index"), ChannelIndex);
    Response->SetStringField(TEXT("duplicated_value"), ValueEcho);
    Response->SetNumberField(TEXT("keys_written"), KeysWritten);
    Response->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("duplicate_keyframe"));
}

// ========================================================================== //
// Sequencer sub-section authoring
//
// Section-level mutations, plus a track-count readback:
//
//   remove_sequence_track             delete any track from a sequence by
//                                    display name OR exact class name
//                                    (UMovieScene::RemoveTrack).
//   add_event_track_section           append a new UMovieSceneEventSection
//                                    to the existing UMovieSceneEventTrack
//                                    (add_event_track_to_sequence creates
//                                    the track itself).
//   add_camera_cut_section            append a new UMovieSceneCameraCutSection
//                                    to the existing UMovieSceneCameraCutTrack
//                                    and bind a camera binding GUID (the
//                                    actor the cut is on).
//   set_audio_section_start_offset    set the StartFrameOffset on an existing
//                                    UMovieSceneAudioSection (skip-into-clip).
//   get_sequence_track_count          flat readback: total track count +
//                                    breakdown by track class (no nested
//                                    track walk, just class names + counts).
//
// The mutators work directly on the ULevelSequence / UMovieScene asset model with the
// pattern of the other section commands: section/track guard + FScopedTransaction +
// Modify() + SaveSequencePackage(). get_sequence_track_count only reads.
// ========================================================================== //

// MCP:remove_sequence_track - Delete any track from a sequence by display name OR exact class name (optionally filtered by track_class).
//   @param sequence_path - Asset path of the Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param track_name - Display name OR class short name of the track to remove.
//   @param track_class - Optional exact class filter (e.g. "MovieSceneFadeTrack") to disambiguate.
//   @returns { sequence_path: string, track_name: string, track_class: string, sections_removed: int, removed: bool, saved: bool }
TSharedPtr<FJsonObject> HandleRemoveSequenceTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequencePathOrName(Params, SequencePath)) { return Missing; }
    FString TrackName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("track_name"), TrackName)) { return Missing; }
    // Optional: filter to a specific track class (otherwise we match by display
    // name OR class short name, first match wins).
    FString ClassFilter;
    Params->TryGetStringField(TEXT("track_class"), ClassFilter);

    ULevelSequence* Sequence = LoadSequenceByPathOrName(SequencePath);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequencePath, MovieScene)) { return Failure; }

    // Find the first track matching by display name OR class name. If a
    // track_class filter is supplied, narrow the search to that exact class
    // (so callers can remove "the UMovieSceneFadeTrack" when multiple tracks
    // share a generic name like "Fade").
    UMovieSceneTrack* Matched = nullptr;
    FString MatchedName;
    FString MatchedClass;
    for (UMovieSceneTrack* Track : MovieScene->GetTracks())
    {
        if (!Track) { continue; }
        const FString TrackClassName = Track->GetClass()->GetName();
        const FString TrackDisplayName = Track->GetTrackName().IsNone() ? FString() : Track->GetTrackName().ToString();
        if (!ClassFilter.IsEmpty() && !TrackClassName.Equals(ClassFilter, ESearchCase::IgnoreCase))
        {
            continue;
        }
        if (TrackDisplayName.Equals(TrackName, ESearchCase::IgnoreCase) || TrackClassName.Equals(TrackName, ESearchCase::IgnoreCase))
        {
            Matched = Track;
            MatchedName = TrackDisplayName.IsEmpty() ? TrackClassName : TrackDisplayName;
            MatchedClass = TrackClassName;
            break;
        }
    }
    if (!Matched)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("No track matching '%s' (class filter '%s') on sequence '%s'"),
                            *TrackName, *ClassFilter, *SequencePath));
    }

    const int32 SectionCountBefore = Matched->GetAllSections().Num();
    const FScopedTransaction Transaction(LOCTEXT("RemoveSequenceTrack", "MCP: Remove Sequence Track"));
    MovieScene->Modify();
    Matched->Modify();
    const bool bRemoved = MovieScene->RemoveTrack(*Matched);
    if (!bRemoved)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("MovieScene::RemoveTrack returned false for '%s'"), *MatchedName));
    }
    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetStringField(TEXT("track_name"), MatchedName);
    Response->SetStringField(TEXT("track_class"), MatchedClass);
    Response->SetNumberField(TEXT("sections_removed"), SectionCountBefore);
    Response->SetBoolField(TEXT("removed"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("remove_sequence_track"));
}

// MCP:add_event_track_section - Append a new UMovieSceneEventSection to the existing UMovieSceneEventTrack on the sequence.
//   @param sequence_path - Asset path of the Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param start_frame - Inclusive start frame for the new event section (default 0).
//   @param end_frame - Exclusive end frame for the new event section (default = start_frame + 1).
//   @returns { sequence_path: string, track_class: string, section_index: int, section_count: int, start_frame: int, end_frame: int, section_class: string, section_added: bool, saved: bool }
TSharedPtr<FJsonObject> HandleAddEventTrackSection(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequencePathOrName(Params, SequencePath)) { return Missing; }
    double StartFrameD = 0.0, EndFrameD = 0.0;
    Params->TryGetNumberField(TEXT("start_frame"), StartFrameD);
    Params->TryGetNumberField(TEXT("end_frame"), EndFrameD);
    const int32 StartFrame = FMath::RoundToInt(StartFrameD);
    const int32 EndFrame = FMath::RoundToInt(EndFrameD);
    if (EndFrame <= StartFrame)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("end_frame (%d) must be greater than start_frame (%d)"), EndFrame, StartFrame));
    }

    ULevelSequence* Sequence = LoadSequenceByPathOrName(SequencePath);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequencePath, MovieScene)) { return Failure; }

    UMovieSceneEventTrack* Track = MovieScene->FindTrack<UMovieSceneEventTrack>();
    if (!Track)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No UMovieSceneEventTrack on this sequence. Call add_event_track_to_sequence first."));
    }

    const FScopedTransaction Transaction(LOCTEXT("AddEventTrackSection", "MCP: Add Event Track Section"));
    Track->Modify();

    // UMovieSceneEventSection constructor + SetRange + AddSection (override on
    // UMovieSceneEventTrack). Outer the section under the track so PostEdit
    // change notifications propagate correctly.
    // UMovieSceneEventSection does NOT have an Initialise() method
    // (only UMovieScene3DTransformSection / similar do); the base ctor + SetRange
    // is enough -- UMovieSceneTrack::AddSection calls into the section's SetRow
    // if the section implements IRowAnimModelExtension (EventSection does).
    UMovieSceneEventSection* NewSection = NewObject<UMovieSceneEventSection>(Track);
    NewSection->SetRange(TRange<FFrameNumber>(FFrameNumber(StartFrame), FFrameNumber(EndFrame)));
    Track->AddSection(*NewSection);

    const bool bSaved = SaveSequencePackage(Sequence);
    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
    Response->SetNumberField(TEXT("section_index"), Track->GetAllSections().Num() - 1);
    Response->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
    Response->SetNumberField(TEXT("start_frame"), StartFrame);
    Response->SetNumberField(TEXT("end_frame"), EndFrame);
    Response->SetStringField(TEXT("section_class"), NewSection->GetClass()->GetName());
    Response->SetBoolField(TEXT("section_added"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_event_track_section"));
}

// MCP:add_camera_cut_section - Append a new UMovieSceneCameraCutSection bound to the given camera binding GUID over a frame range.
//   @param sequence_path - Asset path of the Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param camera_binding_guid - GUID of the camera binding the cut switches to.
//   @param start_frame - Inclusive start frame (default 0).
//   @param end_frame - Exclusive end frame (default = start_frame + 1).
//   @returns { sequence_path: string, track_class: string, camera_binding_guid: string, section_index: int, section_count: int, start_frame: int, end_frame: int, section_added: bool, saved: bool }
TSharedPtr<FJsonObject> HandleAddCameraCutSection(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequencePathOrName(Params, SequencePath)) { return Missing; }
    FString CameraBindingGuidStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("camera_binding_guid"), CameraBindingGuidStr)) { return Missing; }
    FGuid CameraBindingGuid;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentSequencerImpl::ParseGuid(CameraBindingGuidStr, TEXT("camera_binding_guid"), CameraBindingGuid)) { return Invalid; }
    double StartFrameD = 0.0, EndFrameD = 0.0;
    Params->TryGetNumberField(TEXT("start_frame"), StartFrameD);
    Params->TryGetNumberField(TEXT("end_frame"), EndFrameD);
    const int32 StartFrame = FMath::RoundToInt(StartFrameD);
    const int32 EndFrame = FMath::RoundToInt(EndFrameD);
    if (EndFrame <= StartFrame)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("end_frame (%d) must be greater than start_frame (%d)"), EndFrame, StartFrame));
    }

    ULevelSequence* Sequence = LoadSequenceByPathOrName(SequencePath);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequencePath, MovieScene)) { return Failure; }

    UMovieSceneCameraCutTrack* Track = MovieScene->FindTrack<UMovieSceneCameraCutTrack>();
    if (!Track)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No UMovieSceneCameraCutTrack on this sequence. Call add_camera_cut_track first."));
    }

    // Verify the camera binding exists on the sequence.
    if (!MovieScene->FindBinding(CameraBindingGuid))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Camera binding GUID %s not found on this sequence"), *CameraBindingGuidStr));
    }

    const FScopedTransaction Transaction(LOCTEXT("AddCameraCutSection", "MCP: Add Camera Cut Section"));
    Track->Modify();

    // UMovieSceneCameraCutSection is the section type that lives on the
    // camera-cut track. We construct it with the camera binding ID so the
    // cut actually points at the requested camera.
    // This class has no Initialise() override -- the base ctor
    // + SetRange + SetCameraBindingID is sufficient (Track->AddSection wires
    // the row index up via the camera-cut section's IRowAnimModelExtension).
    UMovieSceneCameraCutSection* NewSection = NewObject<UMovieSceneCameraCutSection>(Track);
    NewSection->SetCameraBindingID(FMovieSceneObjectBindingID(CameraBindingGuid));
    NewSection->SetRange(TRange<FFrameNumber>(FFrameNumber(StartFrame), FFrameNumber(EndFrame)));
    Track->AddSection(*NewSection);

    const bool bSaved = SaveSequencePackage(Sequence);
    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
    Response->SetStringField(TEXT("camera_binding_guid"), CameraBindingGuidStr);
    Response->SetNumberField(TEXT("section_index"), Track->GetAllSections().Num() - 1);
    Response->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
    Response->SetNumberField(TEXT("start_frame"), StartFrame);
    Response->SetNumberField(TEXT("end_frame"), EndFrame);
    Response->SetBoolField(TEXT("section_added"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_camera_cut_section"));
}

// MCP:set_audio_section_start_offset - Set the StartFrameOffset on an existing UMovieSceneAudioSection (skip-into-clip).
//   @param sequence_path - Asset path of the Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param section_index - Index of the audio section on the audio track (default 0).
//   @param start_offset_seconds - Optional offset in seconds (converted via display rate).
//   @param start_offset_frames - Optional raw frame offset (use for non-display-rate precision).
//   @returns { sequence_path: string, section_index: int, old_start_offset_frames: int, new_start_offset_frames: int, applied: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetAudioSectionStartOffset(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequencePathOrName(Params, SequencePath)) { return Missing; }
    int32 SectionIndex = 0;
    if (Params->HasField(TEXT("section_index")))
    {
        SectionIndex = static_cast<int32>(Params->GetNumberField(TEXT("section_index")));
    }
    if (!Params->HasField(TEXT("start_offset_seconds")) && !Params->HasField(TEXT("start_offset_frames")))
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: start_offset_seconds (or start_offset_frames)"));
    }
    const double StartOffsetSecondsD = Params->HasField(TEXT("start_offset_seconds"))
        ? Params->GetNumberField(TEXT("start_offset_seconds"))
        : 0.0;
    const double StartOffsetFramesD = Params->HasField(TEXT("start_offset_frames"))
        ? Params->GetNumberField(TEXT("start_offset_frames"))
        : 0.0;

    ULevelSequence* Sequence = LoadSequenceByPathOrName(SequencePath);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequencePath, MovieScene)) { return Failure; }

    UMovieSceneAudioTrack* Track = MovieScene->FindTrack<UMovieSceneAudioTrack>();
    if (!Track)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No UMovieSceneAudioTrack on this sequence. Call add_audio_section first."));
    }
    if (!Track->GetAllSections().IsValidIndex(SectionIndex))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("section_index %d out of range (track has %d audio sections)"),
                            SectionIndex, Track->GetAllSections().Num()));
    }
    UMovieSceneAudioSection* Section = Cast<UMovieSceneAudioSection>(Track->GetAllSections()[SectionIndex]);
    if (!Section)
    {
        return mcp_common::MakeErrorReply(TEXT("Audio section cast failed (unexpected section type)"));
    }

    // The engine accepts the offset as FFrameNumber; we convert from seconds
    // (or accept the raw frame value if start_offset_frames was passed).
    FFrameNumber NewOffset;
    if (Params->HasField(TEXT("start_offset_frames")))
    {
        // FFrameNumber's single-arg ctor is templated on int32 -- explicit cast
        // from int64 (FMath::RoundToInt return type) avoids the C2440.
        NewOffset = FFrameNumber(static_cast<int32>(StartOffsetFramesD));
    }
    else
    {
        const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
        NewOffset = DisplayRate.AsFrameTime(StartOffsetSecondsD).RoundToFrame();
    }

    const FFrameNumber OldOffset = Section->GetStartOffset();
    const FScopedTransaction Transaction(LOCTEXT("SetAudioSectionStartOffset", "MCP: Set Audio Section Start Offset"));
    Section->Modify();
    Section->SetStartOffset(NewOffset);

    const bool bSaved = SaveSequencePackage(Sequence);
    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetNumberField(TEXT("section_index"), SectionIndex);
    Response->SetNumberField(TEXT("old_start_offset_frames"), OldOffset.Value);
    Response->SetNumberField(TEXT("new_start_offset_frames"), NewOffset.Value);
    Response->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_audio_section_start_offset"));
}

// MCP:get_sequence_track_count - Flat readback of the total/master/binding track counts and per-class breakdown.
//   @param sequence_path - Asset path of the Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @returns { sequence_path: string, total_track_count: int, master_track_count: int, binding_track_count: int, total_section_count: int, distinct_class_count: int, class_breakdown: [{class, count}] }
TSharedPtr<FJsonObject> HandleGetSequenceTrackCount(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequencePathOrName(Params, SequencePath)) { return Missing; }

    ULevelSequence* Sequence = LoadSequenceByPathOrName(SequencePath);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequencePath, MovieScene)) { return Failure; }

    // Flat readback. Build a TMap<class_name, count> from MovieScene->GetTracks().
    TMap<FString, int32> ClassCounts;
    int32 MasterCount = 0;
    int32 BindingCount = 0;
    int32 TotalSections = 0;
    const TArray<UMovieSceneTrack*>& AllTracks = MovieScene->GetTracks();
    for (const UMovieSceneTrack* Track : AllTracks)
    {
        if (!Track) { continue; }
        const FString ClassName = Track->GetClass()->GetName();
        ClassCounts.FindOrAdd(ClassName)++;
        const FGuid BindingGuid = Track->FindObjectBindingGuid();
        if (BindingGuid.IsValid()) { ++BindingCount; } else { ++MasterCount; }
        TotalSections += Track->GetAllSections().Num();
    }

    // Build the class_breakdown array sorted by count desc.
    TArray<TSharedPtr<FJsonValue>> ClassArr;
    TArray<TPair<FString, int32>> Sorted;
    for (const TPair<FString, int32>& Pair : ClassCounts)
    {
        Sorted.Add(Pair);
    }
    Sorted.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B) { return A.Value > B.Value; });
    for (const TPair<FString, int32>& Pair : Sorted)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("class"), Pair.Key);
        Obj->SetNumberField(TEXT("count"), Pair.Value);
        ClassArr.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetNumberField(TEXT("total_track_count"), AllTracks.Num());
    Response->SetNumberField(TEXT("master_track_count"), MasterCount);
    Response->SetNumberField(TEXT("binding_track_count"), BindingCount);
    Response->SetNumberField(TEXT("total_section_count"), TotalSections);
    Response->SetNumberField(TEXT("distinct_class_count"), ClassCounts.Num());
    Response->SetArrayField(TEXT("class_breakdown"), ClassArr);
    return mcp_common::MakeOkReply(Response);
}

// ---------------------------------------------------------------------------
// Sequencer property binding + audio curve + weighted value.
// ---------------------------------------------------------------------------

// MCP:bind_actor_property_to_sequence - Bind an editor-world actor to a Level Sequence (possessable) and add a typed property track to its binding.  Combined bind+animate flow.
//   @param sequence_name - Name of the target Level Sequence.
//   @param actor_name - Label (or name) of the actor in the current editor world to bind.
//   @param property_name - Name of the property to animate.
//   @param property_type - One of "Float", "Bool", "Vector", "Color", "Visibility".
//   @param track_label - Optional display label for the new track.
//   @returns { sequence_name: string, actor_name: string, property_name: string, property_type: string, binding_guid: string, track_class: string, section_class: string, binding_created: bool, saved: bool }
TSharedPtr<FJsonObject> HandleBindActorPropertyToSequence(const TSharedPtr<FJsonObject>& Params)
{
    // 1. Finds or creates the actor's possessable with the resolve + bind pattern of
    //    HandleBindActorToSequence (not called directly: the json keys differ).
    //    2. Attaches the typed property track + section through HandleAddPropertyTrack,
    //    the same "bind then animate" contract as the rest of the timeline domain.
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }
    FString PropertyName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property_name"), PropertyName)) { return Missing; }
    FString PropertyTypeRaw;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property_type"), PropertyTypeRaw)) { return Missing; }

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    UWorld* World = nullptr;
    AActor* TargetActor = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::FindActorInEditorWorld(ActorName, World, TargetActor))
    {
        return Failure;
    }

    // UMovieScene has no FindPossessableObjectId(UObject*, UWorld*) (MovieScene.h), and
    // UMovieScene::Possessables is private (MovieScene.h); iterate via the
    // public GetPossessableCount()/GetPossessable(int32) pair (MovieScene.h).
    // Falls back to CreatePossessable on the sequence (MovieSceneSequence.h).
    FGuid BindingGuid;
    const int32 PossessableCount = MovieScene->GetPossessableCount();
    for (int32 Index = 0; Index < PossessableCount; ++Index)
    {
        const FMovieScenePossessable& Poss = MovieScene->GetPossessable(Index);
        if (const UClass* PossClass = Poss.GetLoadedPossessedObjectClass())
        {
            if (PossClass == TargetActor->GetClass())
            {
                BindingGuid = Poss.GetGuid();
                break;
            }
        }
    }
    bool bCreatedBinding = false;
    if (!BindingGuid.IsValid())
    {
        const FScopedTransaction Transaction(LOCTEXT("BindActorProperty", "MCP: Bind Actor Property"));
        MovieScene->Modify();
        BindingGuid = static_cast<UMovieSceneSequence*>(Sequence)->CreatePossessable(TargetActor);
        bCreatedBinding = BindingGuid.IsValid();
    }
    if (!BindingGuid.IsValid())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to bind actor '%s' to sequence"), *ActorName));
    }

    // Delegate to the existing HandleAddPropertyTrack path so the typed track +
    // section creation logic stays in one place.
    TSharedPtr<FJsonObject> TrackRes = HandleAddPropertyTrack(UEAgentSequencerImpl::MakePropertyTrackRequest(
        Params, SequenceName, BindingGuid.ToString(), PropertyTypeRaw, PropertyName));
    if (!TrackRes.IsValid() || !TrackRes->GetBoolField(TEXT("success")))
    {
        return TrackRes.IsValid() ? TrackRes
            : mcp_common::MakeErrorReply(TEXT("HandleAddPropertyTrack returned null"));
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("actor_name"), TargetActor->GetActorLabel());
    Response->SetStringField(TEXT("property_name"), PropertyName);
    Response->SetStringField(TEXT("property_type"), NormalizePropertyType(PropertyTypeRaw));
    Response->SetStringField(TEXT("binding_guid"), BindingGuid.ToString());
    Response->SetStringField(TEXT("track_class"), TrackRes->GetStringField(TEXT("track_class")));
    Response->SetStringField(TEXT("section_class"), TrackRes->GetStringField(TEXT("section_class")));
    Response->SetBoolField(TEXT("binding_created"), bCreatedBinding);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("bind_actor_property_to_sequence"));
}

// MCP:bind_component_property_to_sequence - Bind an editor-world actor's component to a Level Sequence (possessable, parented to actor's binding) and add a typed property track to its binding.
//   @param sequence_path - Asset path of the Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param actor_name - Label (or name) of the actor in the current editor world.
//   @param component_name - Component object name, component class name, or "root".
//   @param property_name - Name of the property to animate.
//   @param property_type - One of "Float", "Bool", "Vector", "Color", "Visibility".
//   @param track_label - Optional display label for the new track.
//   @returns { sequence_path: string, actor_name: string, component_name: string, property_name: string, property_type: string, binding_guid: string, parented_to_actor: bool, track_class: string, section_class: string, saved: bool }
TSharedPtr<FJsonObject> HandleBindComponentPropertyToSequence(const TSharedPtr<FJsonObject>& Params)
{
    // Mirrors HandleBindActorPropertyToSequence but for a component on an actor.
    // Reuses HandleBindComponentToSequence for the possessable + parenting
    // contract, then HandleAddPropertyTrack for the typed track. Components are
    // uniquely named (actor.component), so no separate possessable scan is needed.
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequencePathOrName(Params, SequencePath)) { return Missing; }
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }
    FString ComponentName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("component_name"), ComponentName)) { return Missing; }
    FString PropertyName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property_name"), PropertyName)) { return Missing; }
    FString PropertyTypeRaw;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property_type"), PropertyTypeRaw)) { return Missing; }

    // 1. Bind component -> sequence via the proven HandleBindComponentToSequence.
    TSharedPtr<FJsonObject> BindParams = MakeShared<FJsonObject>();
    BindParams->SetStringField(TEXT("sequence_path"), SequencePath);
    BindParams->SetStringField(TEXT("actor_name"), ActorName);
    BindParams->SetStringField(TEXT("component_name"), ComponentName);
    TSharedPtr<FJsonObject> BindRes = HandleBindComponentToSequence(BindParams);
    if (!BindRes.IsValid() || !BindRes->GetBoolField(TEXT("success")))
    {
        return BindRes.IsValid() ? BindRes
            : mcp_common::MakeErrorReply(TEXT("HandleBindComponentToSequence returned null"));
    }
    // HandleBindComponentToSequence returns its payload nested under "data" (via
    // MakeOkReply), so the binding_guid lives one level deeper than
    // the success flag. Read it from the nested object -- FJsonObject returns
    // empty string if missing, which HandleAddPropertyTrack would then reject.
    const TSharedPtr<FJsonObject>* BindData = nullptr;
    FString BindingGuidStr;
    if (BindRes->TryGetObjectField(TEXT("data"), BindData) && BindData && BindData->IsValid())
    {
        (*BindData)->TryGetStringField(TEXT("binding_guid"), BindingGuidStr);
    }

    // 2. Attach typed property track to that binding.
    // HandleAddPropertyTrack only reads `sequence_name` and finds it by asset name, not
    // by the full /Game/... path, so it gets the trailing asset name of SequencePath.
    FString SequenceNameForTrack = SequencePath;
    {
        int32 SlashIdx = INDEX_NONE;
        if (SequencePath.FindLastChar(TEXT('/'), SlashIdx))
        {
            SequenceNameForTrack = SequencePath.RightChop(SlashIdx + 1);
        }
    }
    TSharedPtr<FJsonObject> Inner = UEAgentSequencerImpl::MakePropertyTrackRequest(
        Params, SequenceNameForTrack, BindingGuidStr, PropertyTypeRaw, PropertyName);
    Inner->SetStringField(TEXT("sequence_path"), SequencePath);
    TSharedPtr<FJsonObject> TrackRes = HandleAddPropertyTrack(Inner);
    if (!TrackRes.IsValid() || !TrackRes->GetBoolField(TEXT("success")))
    {
        return TrackRes.IsValid() ? TrackRes
            : mcp_common::MakeErrorReply(TEXT("HandleAddPropertyTrack returned null"));
    }

    ULevelSequence* Sequence = LoadSequenceByPathOrName(SequencePath);
    const bool bSaved = Sequence ? SaveSequencePackage(Sequence) : false;

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("sequence_path"), SequencePath);
    Response->SetStringField(TEXT("actor_name"), BindRes->GetStringField(TEXT("actor_name")));
    Response->SetStringField(TEXT("component_name"), BindRes->GetStringField(TEXT("component_name")));
    Response->SetStringField(TEXT("property_name"), PropertyName);
    Response->SetStringField(TEXT("property_type"), NormalizePropertyType(PropertyTypeRaw));
    Response->SetStringField(TEXT("binding_guid"), BindingGuidStr);
    Response->SetBoolField(TEXT("parented_to_actor"), BindRes->GetBoolField(TEXT("parented_to_actor")));
    Response->SetStringField(TEXT("track_class"), TrackRes->GetStringField(TEXT("track_class")));
    Response->SetStringField(TEXT("section_class"), TrackRes->GetStringField(TEXT("section_class")));
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("bind_component_property_to_sequence"));
}

namespace UEAgentSequencerImpl
{
    // The SectionIndex-th section of the TrackIndex-th root audio track; returns the error reply when absent.
    TSharedPtr<FJsonObject> FindRootAudioSection(UMovieScene* MovieScene, int32 TrackIndex, int32 SectionIndex,
        UMovieSceneAudioSection*& OutSection)
    {
        int32 AudioSeen = 0;
        UMovieSceneAudioTrack* AudioTrack = nullptr;
        for (UMovieSceneTrack* T : MovieScene->GetTracks())
        {
            if (UMovieSceneAudioTrack* AT = Cast<UMovieSceneAudioTrack>(T))
            {
                if (AudioSeen == TrackIndex) { AudioTrack = AT; break; }
                ++AudioSeen;
            }
        }
        if (!AudioTrack)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Audio track index %d not found"), TrackIndex));
        }
        if (!AudioTrack->GetAllSections().IsValidIndex(SectionIndex))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Section index %d out of range (have %d)"), SectionIndex, AudioTrack->GetAllSections().Num()));
        }
        OutSection = Cast<UMovieSceneAudioSection>(AudioTrack->GetAllSections()[SectionIndex]);
        if (!OutSection)
        {
            return mcp_common::MakeErrorReply(TEXT("Audio section is null"));
        }
        return nullptr;
    }

    // Keys Value at time_seconds, or as the channel's only key at frame 0 without it; returns keys_written.
    int32 KeyAudioChannel(const TSharedPtr<FJsonObject>& Params, UMovieScene* MovieScene, FMovieSceneFloatChannel& Channel, double Value)
    {
        if (Params->HasField(TEXT("time_seconds")))
        {
            const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
            const FFrameRate TickResolution = MovieScene->GetTickResolution();
            const double TimeSeconds = Params->GetNumberField(TEXT("time_seconds"));
            const FFrameNumber Frame = FFrameTime::FromDecimal(TimeSeconds * TickResolution.AsDecimal() * DisplayRate.AsInterval()).GetFrame();
            return Channel.AddLinearKey(Frame, (float)Value);
        }
        Channel.GetData().Reset();
        Channel.AddLinearKey(FFrameNumber(0), (float)Value);
        return 1;
    }
}

// MCP:set_audio_track_volume - Set the SoundVolume channel on an audio section (default value or at a specific time).
//   @param sequence_name - Name of the target Level Sequence.
//   @param audio_track_index - Index of the root audio track (default 0).
//   @param section_index - Index of the section within the audio track (default 0).
//   @param volume - New volume value (0..N; 1 = unity gain).
//   @param time_seconds - Optional time (seconds) for the key. Omit to set the constant value.
//   @returns { sequence_name: string, audio_track_index: int, section_index: int, volume: float, channel_name: string, key_count: int, keys_written: int, saved: bool }
TSharedPtr<FJsonObject> HandleSetAudioTrackVolume(const TSharedPtr<FJsonObject>& Params)
{
    // Resolves the audio section by sequence + audio_track_index + section_index,
    // then writes Volume (default) or Volume-at-time into the FMovieSceneFloatChannel
    // SoundVolume: UMovieSceneAudioSection::SoundVolume
    // (MovieSceneAudioSection.h).
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }
    int32 TrackIndex = 0;
    Params->TryGetNumberField(TEXT("audio_track_index"), TrackIndex);
    int32 SectionIndex = 0;
    Params->TryGetNumberField(TEXT("section_index"), SectionIndex);
    double Volume = 1.0;
    if (!Params->HasField(TEXT("volume")))
    {
        return mcp_common::MissingParameterReply(TEXT("volume"));
    }
    Volume = Params->GetNumberField(TEXT("volume"));

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    UMovieSceneAudioSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::FindRootAudioSection(MovieScene, TrackIndex, SectionIndex, Section))
    {
        return Failure;
    }

    const FScopedTransaction Transaction(LOCTEXT("SetAudioVolume", "MCP: Set Audio Track Volume"));
    Section->Modify();
    FMovieSceneFloatChannel& Channel = const_cast<FMovieSceneFloatChannel&>(Section->GetSoundVolumeChannel());
    const int32 KeysWritten = UEAgentSequencerImpl::KeyAudioChannel(Params, MovieScene, Channel, Volume);

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetNumberField(TEXT("audio_track_index"), TrackIndex);
    Response->SetNumberField(TEXT("section_index"), SectionIndex);
    Response->SetNumberField(TEXT("volume"), Volume);
    Response->SetStringField(TEXT("channel_name"), TEXT("SoundVolume"));
    Response->SetNumberField(TEXT("key_count"), Channel.GetData().GetTimes().Num());
    Response->SetNumberField(TEXT("keys_written"), KeysWritten);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_audio_track_volume"));
}

// MCP:set_audio_track_pitch - Set the PitchMultiplier channel on an audio section (default value or at a specific time).
//   @param sequence_name - Name of the target Level Sequence.
//   @param audio_track_index - Index of the root audio track (default 0).
//   @param section_index - Index of the section within the audio track (default 0).
//   @param pitch - New pitch multiplier (1.0 = normal speed).
//   @param time_seconds - Optional time (seconds) for the key. Omit to set the constant value.
//   @returns { sequence_name: string, audio_track_index: int, section_index: int, pitch: float, channel_name: string, key_count: int, keys_written: int, saved: bool }
TSharedPtr<FJsonObject> HandleSetAudioTrackPitch(const TSharedPtr<FJsonObject>& Params)
{
    // The pitch channel is UMovieSceneAudioSection::PitchMultiplier
    // (MovieSceneAudioSection.h). Channel type is FMovieSceneFloatChannel.
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }
    int32 TrackIndex = 0;
    Params->TryGetNumberField(TEXT("audio_track_index"), TrackIndex);
    int32 SectionIndex = 0;
    Params->TryGetNumberField(TEXT("section_index"), SectionIndex);
    double Pitch = 1.0;
    if (!Params->HasField(TEXT("pitch")))
    {
        return mcp_common::MissingParameterReply(TEXT("pitch"));
    }
    Pitch = Params->GetNumberField(TEXT("pitch"));

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    UMovieSceneAudioSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::FindRootAudioSection(MovieScene, TrackIndex, SectionIndex, Section))
    {
        return Failure;
    }

    const FScopedTransaction Transaction(LOCTEXT("SetAudioPitch", "MCP: Set Audio Track Pitch"));
    Section->Modify();
    FMovieSceneFloatChannel& Channel = const_cast<FMovieSceneFloatChannel&>(Section->GetPitchMultiplierChannel());
    const int32 KeysWritten = UEAgentSequencerImpl::KeyAudioChannel(Params, MovieScene, Channel, Pitch);

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetNumberField(TEXT("audio_track_index"), TrackIndex);
    Response->SetNumberField(TEXT("section_index"), SectionIndex);
    Response->SetNumberField(TEXT("pitch"), Pitch);
    Response->SetStringField(TEXT("channel_name"), TEXT("PitchMultiplier"));
    Response->SetNumberField(TEXT("key_count"), Channel.GetData().GetTimes().Num());
    Response->SetNumberField(TEXT("keys_written"), KeysWritten);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_audio_track_pitch"));
}

// MCP:add_weighted_value_keyframe - Write a single key onto an existing float property track's first section (cubic / linear / constant interp).
//   @param sequence_name - Name of the target Level Sequence.
//   @param binding_guid - GUID of the binding owning the float property track.
//   @param property_name - Name of the float property (default "Weight"; used for response only).
//   @param time_seconds - Time (seconds, >= 0) at which to place the key.
//   @param value - Key value (float).
//   @param arrive_tangent - Optional tangent for cubic interp (default 0).
//   @param leave_tangent - Optional tangent for cubic interp (default 0).
//   @param interp_mode - "cubic" (default), "linear", or "constant".
//   @returns { sequence_name: string, binding_guid: string, property_name: string, time_seconds: float, value: float, interp_mode: string, arrive_tangent: float, leave_tangent: float, key_index: int, key_count: int, saved: bool }
TSharedPtr<FJsonObject> HandleAddWeightedValueKeyframe(const TSharedPtr<FJsonObject>& Params)
{
    // Mirrors HandleAddWeightedValueTrack but writes a single
    // cubic key onto the existing float property track's first section. Uses
    // FMovieSceneFloatChannel::AddCubicKey(time, value, RCTM_Cubic, FMovieSceneTangentData)
    // (MovieSceneFloatChannel.h).
    FString SequenceName, GuidStr, PropertyName;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequenceAndBinding(Params, SequenceName, GuidStr)) { return Missing; }
    if (!Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
    {
        PropertyName = TEXT("Weight");
    }
    double TimeSeconds = 0.0;
    if (!Params->TryGetNumberField(TEXT("time_seconds"), TimeSeconds) || TimeSeconds < 0.0)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: time_seconds (>= 0)"));
    }
    double Value = 1.0;
    if (!Params->HasField(TEXT("value")))
    {
        return mcp_common::MissingParameterReply(TEXT("value"));
    }
    Value = Params->GetNumberField(TEXT("value"));
    double ArriveTangent = 0.0, LeaveTangent = 0.0;
    Params->TryGetNumberField(TEXT("arrive_tangent"), ArriveTangent);
    Params->TryGetNumberField(TEXT("leave_tangent"), LeaveTangent);
    FString InterpMode = TEXT("cubic");
    Params->TryGetStringField(TEXT("interp_mode"), InterpMode);
    const FString InterpLower = InterpMode.ToLower();

    FGuid BindingGuid;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentSequencerImpl::ParseGuid(GuidStr, TEXT("binding_guid"), BindingGuid)) { return Invalid; }

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    UMovieSceneFloatTrack* FloatTrack = MovieScene->FindTrack<UMovieSceneFloatTrack>(BindingGuid);
    if (!FloatTrack || FloatTrack->GetAllSections().Num() == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No float property track on this binding. Call add_property_track (property_type=Float) first."));
    }
    UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(FloatTrack->GetAllSections()[0]);
    if (!Section)
    {
        return mcp_common::MakeErrorReply(TEXT("Float section is null"));
    }

    const FScopedTransaction Transaction(LOCTEXT("AddWeightedKey", "MCP: Add Weighted Value Keyframe"));
    Section->Modify();

    const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
    const FFrameRate TickResolution = MovieScene->GetTickResolution();
    const FFrameNumber Frame = FFrameTime::FromDecimal(TimeSeconds * TickResolution.AsDecimal() * DisplayRate.AsInterval()).GetFrame();

    TArrayView<FMovieSceneFloatChannel*> FloatChannels =
        Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
    if (FloatChannels.Num() == 0 || !FloatChannels[0])
    {
        return mcp_common::MakeErrorReply(TEXT("Float section has no float channel"));
    }
    FMovieSceneFloatChannel& Channel = *FloatChannels[0];

    int32 KeyIndex = INDEX_NONE;
    if (InterpLower == TEXT("linear"))
    {
        KeyIndex = Channel.AddLinearKey(Frame, (float)Value);
    }
    else if (InterpLower == TEXT("constant"))
    {
        KeyIndex = Channel.AddConstantKey(Frame, (float)Value);
    }
    else
    {
        // Cubic (default for "weighted value"). FMovieSceneTangentData has
        // ArriveTangent + LeaveTangent + TangentWeightMode; we keep weight at
        // 1.0 (default). The signature is on MovieSceneFloatChannel.h.
        // FMovieSceneTangentData (MovieSceneCurveChannelCommon.h) has no
        // 4-arg ctor; use default ctor + named-field init for Arrive/Leave tangents and weight mode.
        FMovieSceneTangentData Tangent;
        Tangent.ArriveTangent = (float)ArriveTangent;
        Tangent.LeaveTangent = (float)LeaveTangent;
        Tangent.ArriveTangentWeight = 1.0f;
        Tangent.LeaveTangentWeight = 1.0f;
        Tangent.TangentWeightMode = RCTWM_WeightedNone;
        // There is no RCTM_Cubic; use RCTM_Break (separate Arrive/Leave tangents) which is
        // the closest equivalent when filling both ArriveTangent and LeaveTangent explicitly.
        KeyIndex = Channel.AddCubicKey(Frame, (float)Value, RCTM_Break, Tangent);
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("binding_guid"), GuidStr);
    Response->SetStringField(TEXT("property_name"), PropertyName);
    Response->SetNumberField(TEXT("time_seconds"), TimeSeconds);
    Response->SetNumberField(TEXT("value"), Value);
    Response->SetStringField(TEXT("interp_mode"), InterpLower);
    Response->SetNumberField(TEXT("arrive_tangent"), ArriveTangent);
    Response->SetNumberField(TEXT("leave_tangent"), LeaveTangent);
    Response->SetNumberField(TEXT("key_index"), KeyIndex);
    Response->SetNumberField(TEXT("key_count"), Channel.GetData().GetTimes().Num());
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_weighted_value_keyframe"));
}

// ---------------------------------------------------------------------------
// Advanced master tracks: time-dilation (slomo) + level visibility
// ---------------------------------------------------------------------------

// MCP:add_slomo_track - Add (or return) the root time-dilation (slomo) track with a float section over the playback range, setting the dilation value at a frame. Asset-level/headless (NOT the PIE-only player play-rate).
//   @param sequence_name - Name of the target Level Sequence
//   @param dilation - Optional time-dilation multiplier (default 1.0; <1 slower, >1 faster)
//   @param frame - Optional frame number for the dilation key (default 0)
//   @returns { sequence_name: string, track_class: string, track_added: bool, section_added: bool, section_count: int, section_class: string, dilation: number, saved: bool }
TSharedPtr<FJsonObject> HandleAddSlomoTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }
    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    const FScopedTransaction Transaction(LOCTEXT("AddSlomoTrack", "MCP: Add Slomo Track"));
    MovieScene->Modify();

    bool bTrackCreated = false;
    UMovieSceneSlomoTrack* Track = UEAgentSequencerImpl::FindOrAddTrack<UMovieSceneSlomoTrack>(MovieScene, bTrackCreated);
    if (!Track)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to add slomo track"));
    }

    bool bSectionAdded = false;
    UMovieSceneSection* Section = UEAgentSequencerImpl::FirstOrNewPlaybackSection<UMovieSceneSection>(MovieScene, Track, bSectionAdded);

    double Dilation = 1.0;
    Params->TryGetNumberField(TEXT("dilation"), Dilation);
    int32 FrameNum = 0;
    { double F = 0.0; if (Params->TryGetNumberField(TEXT("frame"), F)) { FrameNum = static_cast<int32>(F); } }
    if (Section)
    {
        // The slomo section is a float section: its single FMovieSceneFloatChannel holds the dilation.
        TArrayView<FMovieSceneFloatChannel*> Channels = Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
        if (Channels.Num() > 0 && Channels[0])
        {
            Channels[0]->GetData().UpdateOrAddKey(FFrameNumber(FrameNum), FMovieSceneFloatValue(static_cast<float>(Dilation)));
        }
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
    Response->SetBoolField(TEXT("track_added"), bTrackCreated);
    Response->SetBoolField(TEXT("section_added"), bSectionAdded);
    Response->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
    if (Section) { Response->SetStringField(TEXT("section_class"), Section->GetClass()->GetName()); }
    Response->SetNumberField(TEXT("dilation"), Dilation);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_slomo_track"));
}

// MCP:add_level_visibility_track - Add a level-visibility track + section controlling streamed-level visibility over the playback range.
//   @param sequence_name - Name of the target Level Sequence
//   @param visible - Optional: true = levels Visible (default), false = Hidden
//   @param level_names - Optional array of streamed-level short names the section controls
//   @returns { sequence_name: string, track_class: string, track_added: bool, section_added: bool, section_count: int, section_class: string, visibility: string, level_count: int, saved: bool }
TSharedPtr<FJsonObject> HandleAddLevelVisibilityTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }
    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    const FScopedTransaction Transaction(LOCTEXT("AddLevelVisTrack", "MCP: Add Level Visibility Track"));
    MovieScene->Modify();

    bool bTrackCreated = false;
    UMovieSceneLevelVisibilityTrack* Track = UEAgentSequencerImpl::FindOrAddTrack<UMovieSceneLevelVisibilityTrack>(MovieScene, bTrackCreated);
    if (!Track)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to add level visibility track"));
    }

    bool bSectionAdded = false;
    UMovieSceneLevelVisibilitySection* Section =
        UEAgentSequencerImpl::FirstOrNewPlaybackSection<UMovieSceneLevelVisibilitySection>(MovieScene, Track, bSectionAdded);

    bool bVisible = true;
    Params->TryGetBoolField(TEXT("visible"), bVisible);
    TArray<FName> LevelNames;
    const TArray<TSharedPtr<FJsonValue>>* LevelArr = nullptr;
    if (Params->TryGetArrayField(TEXT("level_names"), LevelArr) && LevelArr)
    {
        for (const TSharedPtr<FJsonValue>& V : *LevelArr)
        {
            FString S;
            if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty()) { LevelNames.Add(FName(*S)); }
        }
    }
    if (Section)
    {
        Section->SetVisibility(bVisible ? ELevelVisibility::Visible : ELevelVisibility::Hidden);
        if (LevelNames.Num() > 0) { Section->SetLevelNames(LevelNames); }
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
    Response->SetBoolField(TEXT("track_added"), bTrackCreated);
    Response->SetBoolField(TEXT("section_added"), bSectionAdded);
    Response->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
    if (Section)
    {
        Response->SetStringField(TEXT("section_class"), Section->GetClass()->GetName());
        Response->SetStringField(TEXT("visibility"),
            Section->GetVisibility() == ELevelVisibility::Visible ? TEXT("Visible") : TEXT("Hidden"));
        Response->SetNumberField(TEXT("level_count"), Section->GetLevelNames().Num());
    }
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_level_visibility_track"));
}

namespace UEAgentSequencerImpl
{
    // What KeyScalarParameter keyed and added on a material track.
    struct FScalarParameterKey
    {
        FString ParameterName;
        double Value = 0.0;
        bool bKeyAdded = false;
        bool bSectionAdded = false;
        int32 ScalarChannelCount = 0;
    };

    // Keys the optional scalar parameter_name, ensures a playback-range section and counts its float channels.
    FScalarParameterKey KeyScalarParameter(const TSharedPtr<FJsonObject>& Params, UMovieScene* MovieScene, UMovieSceneMaterialTrack* Track)
    {
        FScalarParameterKey Keyed;
        Params->TryGetStringField(TEXT("parameter_name"), Keyed.ParameterName);
        Params->TryGetNumberField(TEXT("value"), Keyed.Value);
        int32 FrameNum = 0;
        { double F = 0.0; if (Params->TryGetNumberField(TEXT("frame"), F)) { FrameNum = static_cast<int32>(F); } }

        if (!Keyed.ParameterName.IsEmpty())
        {
            // AddScalarParameterKey creates a parameter section when the track has none.
            Track->AddScalarParameterKey(FName(*Keyed.ParameterName), FFrameNumber(FrameNum), static_cast<float>(Keyed.Value));
            Keyed.bKeyAdded = true;
        }
        if (Track->GetAllSections().Num() == 0)
        {
            UMovieSceneSection* NewSection = Track->CreateNewSection();
            if (NewSection)
            {
                NewSection->SetRange(MovieScene->GetPlaybackRange());
                Track->AddSection(*NewSection);
                Keyed.bSectionAdded = true;
            }
        }
        if (Track->GetAllSections().Num() > 0 && Track->GetAllSections()[0])
        {
            Keyed.ScalarChannelCount = Track->GetAllSections()[0]->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>().Num();
        }
        return Keyed;
    }
}

// ---------------------------------------------------------------------------
// Material animation (asset-level, headless): the Material Parameter Collection master track and the
// per-binding component material parameter track.
// ---------------------------------------------------------------------------

// MCP:add_material_parameter_collection_track - Add a Material Parameter Collection master track bound to an MPC asset, optionally keying a scalar parameter (asset-level, headless).
//   @param sequence_name - Name of the target Level Sequence
//   @param mpc_path - Object path of an existing UMaterialParameterCollection asset (e.g. /Game/MyGame/MPC_X)
//   @param parameter_name - Optional scalar parameter name to key on the MPC track
//   @param value - Optional scalar value for the keyed parameter (default 0.0)
//   @param frame - Optional frame number for the key (default 0)
//   @returns { sequence_name: string, track_class: string, track_added: bool, mpc_path: string, mpc_name: string, parameter_name: string, value: number, key_added: bool, section_added: bool, scalar_channel_count: int, section_count: int, saved: bool }
TSharedPtr<FJsonObject> HandleAddMaterialParameterCollectionTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }
    FString MpcPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("mpc_path"), MpcPath)) { return Missing; }
    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    UMaterialParameterCollection* Mpc = LoadObject<UMaterialParameterCollection>(nullptr, *MpcPath);
    if (!Mpc)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Material Parameter Collection '%s' not found"), *MpcPath));
    }

    const FScopedTransaction Transaction(LOCTEXT("AddMpcTrack", "MCP: Add Material Parameter Collection Track"));
    MovieScene->Modify();

    // Idempotent: reuse an existing MPC track already bound to this same collection.
    bool bTrackCreated = false;
    UMovieSceneMaterialParameterCollectionTrack* Track = nullptr;
    for (UMovieSceneTrack* T : MovieScene->GetTracks())
    {
        UMovieSceneMaterialParameterCollectionTrack* MpcTrack = Cast<UMovieSceneMaterialParameterCollectionTrack>(T);
        if (MpcTrack && MpcTrack->MPC == Mpc) { Track = MpcTrack; break; }
    }
    if (!Track)
    {
        Track = MovieScene->AddTrack<UMovieSceneMaterialParameterCollectionTrack>();
        bTrackCreated = (Track != nullptr);
    }
    if (!Track)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to add MPC track"));
    }
    Track->MPC = Mpc;

    const UEAgentSequencerImpl::FScalarParameterKey Keyed = UEAgentSequencerImpl::KeyScalarParameter(Params, MovieScene, Track);

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
    Response->SetBoolField(TEXT("track_added"), bTrackCreated);
    Response->SetStringField(TEXT("mpc_path"), MpcPath);
    Response->SetStringField(TEXT("mpc_name"), Mpc->GetName());
    Response->SetStringField(TEXT("parameter_name"), Keyed.ParameterName);
    Response->SetNumberField(TEXT("value"), Keyed.Value);
    Response->SetBoolField(TEXT("key_added"), Keyed.bKeyAdded);
    Response->SetBoolField(TEXT("section_added"), Keyed.bSectionAdded);
    Response->SetNumberField(TEXT("scalar_channel_count"), Keyed.ScalarChannelCount);
    Response->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_material_parameter_collection_track"));
}

// MCP:add_component_material_track - Adds a component material parameter track for one indexed material slot to an object binding of a Level Sequence (first asset-name match under /Game), or reuses the binding's existing track for that slot, and saves the sequence. With parameter_name it keys that scalar parameter at frame (a tick-resolution frame); without it, it only adds a section spanning the playback range when the track has none. An unknown binding_guid is an error. For a Material Parameter Collection use add_material_parameter_collection_track; for other animated properties use add_property_track.
//   @param sequence_name - Name of the target Level Sequence
//   @param binding_guid - GUID of the actor/component object binding to attach the material track to
//   @param material_index - Optional indexed material slot the track animates (default 0)
//   @param parameter_name - Optional scalar material parameter name to key
//   @param value - Optional scalar value for the keyed parameter (default 0.0)
//   @param frame - Optional frame number for the key (default 0)
//   @returns { sequence_name: string, binding_guid: string, track_class: string, track_added: bool, material_index: int, material_type: string, parameter_name: string, value: number, key_added: bool, section_added: bool, scalar_channel_count: int, section_count: int, saved: bool }
TSharedPtr<FJsonObject> HandleAddComponentMaterialTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName, GuidStr;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequenceAndBinding(Params, SequenceName, GuidStr)) { return Missing; }
    FGuid BindingGuid;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentSequencerImpl::ParseGuid(GuidStr, TEXT("binding_guid"), BindingGuid)) { return Invalid; }
    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }
    if (!UEAgentSequencerImpl::IsKnownBinding(MovieScene, BindingGuid))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Binding '%s' not found in sequence"), *GuidStr));
    }

    int32 MaterialIndex = 0;
    { double F = 0.0; if (Params->TryGetNumberField(TEXT("material_index"), F)) { MaterialIndex = static_cast<int32>(F); } }

    const FScopedTransaction Transaction(LOCTEXT("AddComponentMaterialTrack", "MCP: Add Component Material Track"));
    MovieScene->Modify();

    // Idempotent: reuse an existing component material track on this binding for the same indexed slot.
    bool bTrackCreated = false;
    UMovieSceneComponentMaterialTrack* Track = nullptr;
    const TArray<UMovieSceneTrack*> ExistingTracks = MovieScene->FindTracks(UMovieSceneComponentMaterialTrack::StaticClass(), BindingGuid);
    for (UMovieSceneTrack* T : ExistingTracks)
    {
        UMovieSceneComponentMaterialTrack* MatTrack = Cast<UMovieSceneComponentMaterialTrack>(T);
        if (MatTrack && MatTrack->GetMaterialInfo().MaterialSlotIndex == MaterialIndex
            && MatTrack->GetMaterialInfo().MaterialType == EComponentMaterialType::IndexedMaterial)
        {
            Track = MatTrack; break;
        }
    }
    if (!Track)
    {
        Track = MovieScene->AddTrack<UMovieSceneComponentMaterialTrack>(BindingGuid);
        if (Track)
        {
            FComponentMaterialInfo Info;
            Info.MaterialSlotIndex = MaterialIndex;
            Info.MaterialType = EComponentMaterialType::IndexedMaterial;
            Track->SetMaterialInfo(Info);
            bTrackCreated = true;
        }
    }
    if (!Track)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to add component material track"));
    }

    const UEAgentSequencerImpl::FScalarParameterKey Keyed = UEAgentSequencerImpl::KeyScalarParameter(Params, MovieScene, Track);

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("binding_guid"), GuidStr);
    Response->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
    Response->SetBoolField(TEXT("track_added"), bTrackCreated);
    Response->SetNumberField(TEXT("material_index"), Track->GetMaterialInfo().MaterialSlotIndex);
    Response->SetStringField(TEXT("material_type"), TEXT("IndexedMaterial"));
    Response->SetStringField(TEXT("parameter_name"), Keyed.ParameterName);
    Response->SetNumberField(TEXT("value"), Keyed.Value);
    Response->SetBoolField(TEXT("key_added"), Keyed.bKeyAdded);
    Response->SetBoolField(TEXT("section_added"), Keyed.bSectionAdded);
    Response->SetNumberField(TEXT("scalar_channel_count"), Keyed.ScalarChannelCount);
    Response->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_component_material_track"));
}

// ---------------------------------------------------------------------------
// Keyframe channel helpers (set_keyframe_interpolation and the binding-channel
// keyframe commands)
// ---------------------------------------------------------------------------

namespace
{
    // Interp/tangent enum -> string.
    FString SeqInterpToStr(ERichCurveInterpMode M)
    {
        switch (M) { case RCIM_Cubic: return TEXT("Cubic"); case RCIM_Linear: return TEXT("Linear");
        case RCIM_Constant: return TEXT("Constant"); default: return TEXT("None"); }
    }
    FString SeqTangentToStr(ERichCurveTangentMode M)
    {
        switch (M) { case RCTM_Auto: return TEXT("Auto"); case RCTM_User: return TEXT("User");
        case RCTM_Break: return TEXT("Break"); default: return TEXT("None"); }
    }

    // Applies the optional interp/tangent params to a Double/Float key; false + OutError on an invalid mode.
    template <typename ValueType>
    bool SeqApplyInterpEdit(const TSharedPtr<FJsonObject>& Params, ValueType& Key, FString& OutError)
    {
        FString InterpModeStr;
        if (Params->TryGetStringField(TEXT("interp_mode"), InterpModeStr) && !InterpModeStr.IsEmpty())
        {
            if (InterpModeStr.Equals(TEXT("Cubic"), ESearchCase::IgnoreCase))         { Key.InterpMode = RCIM_Cubic; }
            else if (InterpModeStr.Equals(TEXT("Linear"), ESearchCase::IgnoreCase))   { Key.InterpMode = RCIM_Linear; }
            else if (InterpModeStr.Equals(TEXT("Constant"), ESearchCase::IgnoreCase)) { Key.InterpMode = RCIM_Constant; }
            else
            {
                OutError = FString::Printf(TEXT("Unknown interp_mode '%s' (expected Cubic|Linear|Constant)"), *InterpModeStr);
                return false;
            }
        }
        FString TangentModeStr;
        if (Params->TryGetStringField(TEXT("tangent_mode"), TangentModeStr) && !TangentModeStr.IsEmpty())
        {
            if (TangentModeStr.Equals(TEXT("Auto"), ESearchCase::IgnoreCase))        { Key.TangentMode = RCTM_Auto; }
            else if (TangentModeStr.Equals(TEXT("User"), ESearchCase::IgnoreCase))   { Key.TangentMode = RCTM_User; }
            else if (TangentModeStr.Equals(TEXT("Break"), ESearchCase::IgnoreCase))  { Key.TangentMode = RCTM_Break; }
            else
            {
                OutError = FString::Printf(TEXT("Unknown tangent_mode '%s' (expected Auto|User|Break)"), *TangentModeStr);
                return false;
            }
        }
        { double F = 0.0; if (Params->TryGetNumberField(TEXT("arrive_tangent"), F)) { Key.Tangent.ArriveTangent = static_cast<float>(F); } }
        { double F = 0.0; if (Params->TryGetNumberField(TEXT("leave_tangent"), F))  { Key.Tangent.LeaveTangent  = static_cast<float>(F); } }
        return true;
    }

    // Resolve the requested channel kind from the "channel_type" param (default "double").
    // Out params report which kind was selected so callers branch on exactly one.
    enum class ESeqChannelKind : uint8 { Double, Float, Bool };
    bool SeqParseChannelKind(const TSharedPtr<FJsonObject>& Params, ESeqChannelKind& OutKind, FString& OutError)
    {
        FString Raw;
        Params->TryGetStringField(TEXT("channel_type"), Raw);
        const FString Lower = Raw.ToLower();
        if (Lower.IsEmpty() || Lower == TEXT("double") || Lower == TEXT("transform"))
        {
            OutKind = ESeqChannelKind::Double; return true;
        }
        if (Lower == TEXT("float") || Lower == TEXT("scalar") || Lower == TEXT("color"))
        {
            OutKind = ESeqChannelKind::Float; return true;
        }
        if (Lower == TEXT("bool") || Lower == TEXT("boolean") || Lower == TEXT("visibility"))
        {
            OutKind = ESeqChannelKind::Bool; return true;
        }
        OutError = FString::Printf(TEXT("Unknown channel_type '%s' (expected double|float|bool)"), *Raw);
        return false;
    }

    // The channel_type name the replies echo for a channel kind.
    FString SeqChannelKindName(ESeqChannelKind Kind)
    {
        switch (Kind)
        {
        case ESeqChannelKind::Float: return TEXT("float");
        case ESeqChannelKind::Bool:  return TEXT("bool");
        default:                     return TEXT("double");
        }
    }

    // Read the optional channel_index param (default 0).
    int32 SeqReadChannelIndex(const TSharedPtr<FJsonObject>& Params)
    {
        int32 ChannelIndex = 0;
        Params->TryGetNumberField(TEXT("channel_index"), ChannelIndex);
        return ChannelIndex;
    }

    // Locate a key on a channel-data view by "index" (explicit) or "frame" (via FindKey
    // with optional tolerance). Returns the key index, or INDEX_NONE; sets OutError.
    // Takes the MUTABLE TMovieSceneChannelData<T> by non-const ref: its base
    // FMovieSceneChannelData::FindKey is non-const (MovieSceneChannelData.h).
    template <typename ChannelDataType>
    int32 SeqResolveKeyIndex(ChannelDataType& Data, const TSharedPtr<FJsonObject>& Params, FString& OutError)
    {
        const TArrayView<const FFrameNumber> Times = Data.GetTimes();
        int32 ExplicitIndex = INDEX_NONE;
        if (Params->TryGetNumberField(TEXT("index"), ExplicitIndex))
        {
            if (ExplicitIndex < 0 || ExplicitIndex >= Times.Num())
            {
                OutError = FString::Printf(TEXT("key index %d out of range (channel has %d keys)"), ExplicitIndex, Times.Num());
                return INDEX_NONE;
            }
            return ExplicitIndex;
        }
        double FrameD = 0.0;
        if (!Params->TryGetNumberField(TEXT("frame"), FrameD))
        {
            OutError = TEXT("Missing key locator: provide 'index' or 'frame'");
            return INDEX_NONE;
        }
        int32 ToleranceI = 0;
        Params->TryGetNumberField(TEXT("tolerance"), ToleranceI);
        const int32 KeyIndex = Data.FindKey(FFrameNumber(static_cast<int32>(FrameD)), FFrameNumber(ToleranceI));
        if (KeyIndex == INDEX_NONE)
        {
            OutError = FString::Printf(TEXT("No key found at frame %d (tolerance %d)"), static_cast<int32>(FrameD), ToleranceI);
        }
        return KeyIndex;
    }

    // The section's ChannelIndex-th ChannelType channel, or nullptr with OutError naming Kind ("Double"...).
    template <typename ChannelType>
    ChannelType* SeqChannelAt(UMovieSceneSection* Section, int32 ChannelIndex, const TCHAR* Kind, FString& OutError)
    {
        TArrayView<ChannelType*> Channels = Section->GetChannelProxy().GetChannels<ChannelType>();
        if (ChannelIndex < 0 || ChannelIndex >= Channels.Num() || !Channels[ChannelIndex])
        {
            OutError = FString::Printf(TEXT("%s channel index %d out of range (section has %d %s channels)"),
                Kind, ChannelIndex, Channels.Num(), *FString(Kind).ToLower());
            return nullptr;
        }
        return Channels[ChannelIndex];
    }

    // Runs Edit(Data, KeyIndex) on the key 'index'/'frame' locates in that channel; the error reply when either is missing.
    template <typename ChannelType, typename EditType>
    TSharedPtr<FJsonObject> SeqEditKey(const TSharedPtr<FJsonObject>& Params, UMovieSceneSection* Section, int32 ChannelIndex,
        const TCHAR* Kind, EditType&& Edit)
    {
        FString Error;
        ChannelType* Channel = SeqChannelAt<ChannelType>(Section, ChannelIndex, Kind, Error);
        if (!Channel)
        {
            return mcp_common::MakeErrorReply(Error);
        }
        auto Data = Channel->GetData();
        const int32 KeyIndex = SeqResolveKeyIndex(Data, Params, Error);
        if (KeyIndex == INDEX_NONE)
        {
            return mcp_common::MakeErrorReply(Error);
        }
        return Edit(Data, KeyIndex);
    }

    // SeqEditKey on the channel kind that channel_type selected.
    template <typename EditType>
    TSharedPtr<FJsonObject> SeqEditKeyOfKind(const TSharedPtr<FJsonObject>& Params, UMovieSceneSection* Section, ESeqChannelKind Kind,
        int32 ChannelIndex, EditType&& Edit)
    {
        switch (Kind)
        {
        case ESeqChannelKind::Double: return SeqEditKey<FMovieSceneDoubleChannel>(Params, Section, ChannelIndex, TEXT("Double"), Edit);
        case ESeqChannelKind::Float:  return SeqEditKey<FMovieSceneFloatChannel>(Params, Section, ChannelIndex, TEXT("Float"), Edit);
        default:                      return SeqEditKey<FMovieSceneBoolChannel>(Params, Section, ChannelIndex, TEXT("Bool"), Edit);
        }
    }

    // One JSON object per key of a double or float channel, with its interpolation and tangents.
    template <typename ChannelType>
    TArray<TSharedPtr<FJsonValue>> SeqCurveKeysToJson(ChannelType& Channel, const FFrameRate& TickResolution)
    {
        using ValueType = typename ChannelType::ChannelValueType;
        const TMovieSceneChannelData<const ValueType> Data = Channel.GetData();
        const TArrayView<const FFrameNumber> Times = Data.GetTimes();
        const TArrayView<const ValueType> Values = Data.GetValues();
        TArray<TSharedPtr<FJsonValue>> Keyframes;
        for (int32 i = 0; i < Times.Num(); ++i)
        {
            const ValueType& V = Values[i];
            TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>();
            K->SetNumberField(TEXT("index"), i);
            K->SetNumberField(TEXT("frame"), Times[i].Value);
            K->SetNumberField(TEXT("time_seconds"), TickResolution.AsSeconds(FFrameTime(Times[i])));
            K->SetNumberField(TEXT("value"), V.Value);
            K->SetStringField(TEXT("interp_mode"), SeqInterpToStr(V.InterpMode.GetValue()));
            K->SetStringField(TEXT("tangent_mode"), SeqTangentToStr(V.TangentMode.GetValue()));
            K->SetNumberField(TEXT("arrive_tangent"), V.Tangent.ArriveTangent);
            K->SetNumberField(TEXT("leave_tangent"), V.Tangent.LeaveTangent);
            Keyframes.Add(MakeShared<FJsonValueObject>(K));
        }
        return Keyframes;
    }
}

// MCP:set_keyframe_interpolation - Set the interpolation mode, tangent mode and tangents of an existing keyframe on a master float track's channel (asset-level, headless).
//   @param sequence_name - Name of the target Level Sequence
//   @param track_class - Class-name fragment of the master track whose float channel to edit (e.g. "Slomo", "Fade")
//   @param frame - Frame number of the existing key to modify
//   @param interp_mode - Optional interpolation: "Cubic" | "Linear" | "Constant" (default leaves unchanged)
//   @param tangent_mode - Optional tangent mode (Cubic only): "Auto" | "User" | "Break" (default leaves unchanged)
//   @param channel_index - Optional float-channel index on the section (default 0)
//   @param arrive_tangent - Optional arrive tangent value (Cubic)
//   @param leave_tangent - Optional leave tangent value (Cubic)
//   @returns { sequence_name: string, track_class: string, frame: int, key_found: bool, channel_index: int, interp_mode: string, tangent_mode: string, arrive_tangent: number, leave_tangent: number, saved: bool }
TSharedPtr<FJsonObject> HandleSetKeyframeInterpolation(const TSharedPtr<FJsonObject>& Params)
{
    FString SequenceName, TrackClassFragment;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sequence_name"), SequenceName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("track_class"), TrackClassFragment)) { return Missing; }
    double FrameD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("frame"), FrameD)) { return Missing; }
    const int32 FrameNum = static_cast<int32>(FrameD);

    ULevelSequence* Sequence = LevelSequenceUtils::FindByName(SequenceName);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequenceName, MovieScene)) { return Failure; }

    // Resolve the first master track whose class name contains the requested fragment.
    UMovieSceneTrack* TargetTrack = nullptr;
    for (UMovieSceneTrack* T : MovieScene->GetTracks())
    {
        if (T && T->GetClass()->GetName().Contains(TrackClassFragment))
        {
            TargetTrack = T; break;
        }
    }
    if (!TargetTrack)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("No master track matching class '%s' found"), *TrackClassFragment));
    }

    int32 ChannelIndex = 0;
    { double F = 0.0; if (Params->TryGetNumberField(TEXT("channel_index"), F)) { ChannelIndex = static_cast<int32>(F); } }

    UMovieSceneSection* Section = (TargetTrack->GetAllSections().Num() > 0) ? TargetTrack->GetAllSections()[0] : nullptr;
    if (!Section)
    {
        return mcp_common::MakeErrorReply(TEXT("Track has no section to edit"));
    }
    FString Error;
    FMovieSceneFloatChannel* Channel = SeqChannelAt<FMovieSceneFloatChannel>(Section, ChannelIndex, TEXT("Float"), Error);
    if (!Channel)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    TMovieSceneChannelData<FMovieSceneFloatValue> ChannelData = Channel->GetData();
    const int32 KeyIndex = ChannelData.FindKey(FFrameNumber(FrameNum), 0);
    if (KeyIndex == INDEX_NONE)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("No key found at frame %d on channel %d"), FrameNum, ChannelIndex));
    }

    const FScopedTransaction Transaction(LOCTEXT("SetKeyframeInterp", "MCP: Set Keyframe Interpolation"));
    Section->Modify();

    TArrayView<FMovieSceneFloatValue> Values = ChannelData.GetValues();
    FMovieSceneFloatValue& Key = Values[KeyIndex];

    if (!SeqApplyInterpEdit(Params, Key, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // Capture the final stored values for an honest read-back before saving.
    const ERichCurveInterpMode FinalInterp = Key.InterpMode.GetValue();
    const ERichCurveTangentMode FinalTangent = Key.TangentMode.GetValue();
    const float FinalArrive = Key.Tangent.ArriveTangent;
    const float FinalLeave = Key.Tangent.LeaveTangent;

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewNamePayload(SequenceName);
    Response->SetStringField(TEXT("track_class"), TargetTrack->GetClass()->GetName());
    Response->SetNumberField(TEXT("frame"), FrameNum);
    Response->SetBoolField(TEXT("key_found"), true);
    Response->SetNumberField(TEXT("channel_index"), ChannelIndex);
    Response->SetStringField(TEXT("interp_mode"), SeqInterpToStr(FinalInterp));
    Response->SetStringField(TEXT("tangent_mode"), SeqTangentToStr(FinalTangent));
    Response->SetNumberField(TEXT("arrive_tangent"), FinalArrive);
    Response->SetNumberField(TEXT("leave_tangent"), FinalLeave);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_keyframe_interpolation"));
}

// ===========================================================================
// Section-property setters
//
// Overwrite-setters for the typed payload of five section types that the
// add_*_section/track commands create. All are naturally idempotent
// (writing the same value twice = same value) and operate on
// EXISTING sections (resolve -> FScopedTransaction + Section->Modify() -> set typed
// fields -> SaveSequencePackage), mirroring the section easing/blend setters above.
//
// Two section-resolution flavours are in play:
//   * binding-attached sections (transform, skeletal-animation) reuse the existing
//     ResolveSectionFromParams (binding_guid + track_type + section_index).
//   * ROOT/master-track sections (sub, shot, audio) carry NO binding GUID, so they
//     need ResolveMasterSectionFromParams (resolve track by UClass, then section).
// ===========================================================================

// Helper: resolve a section on a ROOT (master) track (sub/shot/audio/etc.), which carries no binding GUID
// and is therefore out of reach of the binding-scoped ResolveSectionFromParams. Resolves
// sequence_path/sequence_name -> the first master track of TrackClass -> section_index (default 0), with
// the same error-reply shape as ResolveSectionFromParams. bExactClass: when true, the track's class must
// equal TrackClass exactly (so UMovieSceneSubTrack does not match its subclass
// UMovieSceneCinematicShotTrack); when false, IsA(TrackClass) matches subclasses too.
TSharedPtr<FJsonObject> ResolveMasterSectionFromParams(
    const TSharedPtr<FJsonObject>& Params, UClass* TrackClass, bool bExactClass,
    ULevelSequence*& OutSequence, UMovieSceneSection*& OutSection)
{
    OutSequence = nullptr;
    OutSection = nullptr;

    if (!TrackClass)
    {
        return mcp_common::MakeErrorReply(TEXT("Internal error: null TrackClass for master-section resolve"));
    }

    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequencePath(Params, SequencePath))
    {
        return Missing;
    }

    OutSequence = LoadSequenceByPathOrName(SequencePath);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(OutSequence, SequencePath, MovieScene))
    {
        return Failure;
    }

    // Find the first ROOT track of TrackClass. MovieScene->GetTracks() returns only the
    // master (no-binding) tracks, so this never matches a binding track. bExactClass
    // forces GetClass()==TrackClass so a UMovieSceneSubTrack lookup does NOT grab its
    // subclass UMovieSceneCinematicShotTrack (and vice-versa).
    UMovieSceneTrack* Track = nullptr;
    for (UMovieSceneTrack* T : MovieScene->GetTracks())
    {
        if (!T) { continue; }
        const bool bMatch = bExactClass ? (T->GetClass() == TrackClass) : T->IsA(TrackClass);
        if (bMatch)
        {
            Track = T;
            break;
        }
    }
    if (!Track)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("No root track of class '%s' on this sequence"), *TrackClass->GetName()));
    }

    int32 SectionIndex = 0;
    Params->TryGetNumberField(TEXT("section_index"), SectionIndex);

    return UEAgentSequencerImpl::SectionAtIndex(Track, SectionIndex, OutSection);
}

// Helper: parse one transform-channel token into EMovieSceneTransformChannel.
// EMovieSceneTransformChannel (MovieSceneSection.h) is a plain `enum class : uint32`,
// NOT a UENUM, so StaticEnum<>() is unavailable -- the parse is hand-rolled. Accepts the
// individual axes, the grouped names, and the aggregates (case/underscore-insensitive).
static bool ParseTransformChannelToken(const FString& Token, EMovieSceneTransformChannel& Out)
{
    FString T = Token.TrimStartAndEnd().Replace(TEXT("_"), TEXT("")).ToLower();
    if (T.IsEmpty()) { return false; }

    if (T == TEXT("translationx") || T == TEXT("locationx") || T == TEXT("posx")) { Out = EMovieSceneTransformChannel::TranslationX; return true; }
    if (T == TEXT("translationy") || T == TEXT("locationy") || T == TEXT("posy")) { Out = EMovieSceneTransformChannel::TranslationY; return true; }
    if (T == TEXT("translationz") || T == TEXT("locationz") || T == TEXT("posz")) { Out = EMovieSceneTransformChannel::TranslationZ; return true; }
    if (T == TEXT("translation") || T == TEXT("location") || T == TEXT("position")) { Out = EMovieSceneTransformChannel::Translation; return true; }

    if (T == TEXT("rotationx") || T == TEXT("rotx") || T == TEXT("roll"))  { Out = EMovieSceneTransformChannel::RotationX; return true; }
    if (T == TEXT("rotationy") || T == TEXT("roty") || T == TEXT("pitch")) { Out = EMovieSceneTransformChannel::RotationY; return true; }
    if (T == TEXT("rotationz") || T == TEXT("rotz") || T == TEXT("yaw"))   { Out = EMovieSceneTransformChannel::RotationZ; return true; }
    if (T == TEXT("rotation") || T == TEXT("rotator"))                     { Out = EMovieSceneTransformChannel::Rotation; return true; }

    if (T == TEXT("scalex")) { Out = EMovieSceneTransformChannel::ScaleX; return true; }
    if (T == TEXT("scaley")) { Out = EMovieSceneTransformChannel::ScaleY; return true; }
    if (T == TEXT("scalez")) { Out = EMovieSceneTransformChannel::ScaleZ; return true; }
    if (T == TEXT("scale"))  { Out = EMovieSceneTransformChannel::Scale; return true; }

    if (T == TEXT("weight")) { Out = EMovieSceneTransformChannel::Weight; return true; }

    if (T == TEXT("alltransform") || T == TEXT("trs")) { Out = EMovieSceneTransformChannel::AllTransform; return true; }
    if (T == TEXT("all"))  { Out = EMovieSceneTransformChannel::All; return true; }
    if (T == TEXT("none")) { Out = EMovieSceneTransformChannel::None; return true; }
    return false;
}

// Helper: collect requested channels from a "channels" param (JSON string array OR a
// comma-separated string). OutChannels is the OR of the parsed tokens; OutBad lists
// any unrecognised token so the caller can return an honest error.
static EMovieSceneTransformChannel ParseTransformChannels(
    const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutBad)
{
    OutBad.Reset();
    EMovieSceneTransformChannel Acc = EMovieSceneTransformChannel::None;

    auto Accumulate = [&](const FString& Tok)
    {
        if (Tok.TrimStartAndEnd().IsEmpty()) { return; }
        EMovieSceneTransformChannel Parsed;
        if (ParseTransformChannelToken(Tok, Parsed)) { Acc |= Parsed; }
        else { OutBad.Add(Tok); }
    };

    const TArray<TSharedPtr<FJsonValue>>* ArrayField = nullptr;
    if (Params->TryGetArrayField(TEXT("channels"), ArrayField) && ArrayField)
    {
        for (const TSharedPtr<FJsonValue>& V : *ArrayField)
        {
            if (V.IsValid()) { Accumulate(V->AsString()); }
        }
        return Acc;
    }

    FString CsvField;
    if (Params->TryGetStringField(TEXT("channels"), CsvField) && !CsvField.IsEmpty())
    {
        TArray<FString> Tokens;
        CsvField.ParseIntoArray(Tokens, TEXT(","), true);
        for (const FString& Tok : Tokens) { Accumulate(Tok); }
    }
    return Acc;
}

// Helper: render an EMovieSceneTransformChannel mask into a JSON string array of the
// enabled atomic axes (+ weight) for an honest, granular read-back.
static TArray<TSharedPtr<FJsonValue>> TransformChannelsToJsonArray(EMovieSceneTransformChannel Channels)
{
    TArray<TSharedPtr<FJsonValue>> Arr;
    auto Has = [&](EMovieSceneTransformChannel Bit)
    {
        return EnumHasAllFlags(Channels, Bit);
    };
    if (Has(EMovieSceneTransformChannel::TranslationX)) { Arr.Add(MakeShared<FJsonValueString>(TEXT("TranslationX"))); }
    if (Has(EMovieSceneTransformChannel::TranslationY)) { Arr.Add(MakeShared<FJsonValueString>(TEXT("TranslationY"))); }
    if (Has(EMovieSceneTransformChannel::TranslationZ)) { Arr.Add(MakeShared<FJsonValueString>(TEXT("TranslationZ"))); }
    if (Has(EMovieSceneTransformChannel::RotationX))    { Arr.Add(MakeShared<FJsonValueString>(TEXT("RotationX"))); }
    if (Has(EMovieSceneTransformChannel::RotationY))    { Arr.Add(MakeShared<FJsonValueString>(TEXT("RotationY"))); }
    if (Has(EMovieSceneTransformChannel::RotationZ))    { Arr.Add(MakeShared<FJsonValueString>(TEXT("RotationZ"))); }
    if (Has(EMovieSceneTransformChannel::ScaleX))       { Arr.Add(MakeShared<FJsonValueString>(TEXT("ScaleX"))); }
    if (Has(EMovieSceneTransformChannel::ScaleY))       { Arr.Add(MakeShared<FJsonValueString>(TEXT("ScaleY"))); }
    if (Has(EMovieSceneTransformChannel::ScaleZ))       { Arr.Add(MakeShared<FJsonValueString>(TEXT("ScaleZ"))); }
    if (Has(EMovieSceneTransformChannel::Weight))       { Arr.Add(MakeShared<FJsonValueString>(TEXT("Weight"))); }
    return Arr;
}

// Helper: write a FMovieSceneTimeWarpVariant constant play-rate JSON read-back. The
// variant only yields a fixed double via AsFixedPlayRate() when GetType()==FixedPlayRate,
// which check()-asserts otherwise -- so guard the read. Set(double) treats ~0 as a
// FixedTime (NOT a play rate), so callers reject near-zero before setting.
static void WriteTimeWarpReadback(const TSharedPtr<FJsonObject>& Response, const FString& Key,
                                  const FMovieSceneTimeWarpVariant& Variant)
{
    if (Variant.GetType() == EMovieSceneTimeWarpType::FixedPlayRate)
    {
        Response->SetNumberField(Key, Variant.AsFixedPlayRate());
    }
    else
    {
        // Non-constant warp (loop/custom/etc.) -- report null so the read-back is honest
        // rather than tripping the AsFixedPlayRate() check().
        Response->SetField(Key, MakeShared<FJsonValueNull>());
    }
}

// MCP:set_sub_section_parameters - Set the playback parameters on a subsequence OR cinematic-shot section (both are UMovieSceneSubSection).  Writes the public FMovieSceneSectionParameters fields directly; bCanLoop gates the loop offsets so it is applied first.
//   @param sequence_path - Full /Game path OR bare asset name of the OUTER Level Sequence that owns the sub/shot track.
//   @param sequence_name - Alias for sequence_path.
//   @param track_type - Optional master-track filter: "subsequence" (default) or "shot" (cinematic shot track).
//   @param section_index - Index of the section on the resolved master track (default 0).
//   @param start_frame_offset - StartFrameOffset in tick frames (omit to leave unchanged).
//   @param end_frame_offset - EndFrameOffset in tick frames (omit to leave unchanged).
//   @param first_loop_start_frame_offset - FirstLoopStartFrameOffset in tick frames; only meaningful when can_loop is true (omit to leave unchanged).
//   @param hierarchical_bias - HierarchicalBias (int; default engine value 100; omit to leave unchanged).
//   @param can_loop [bool] - bCanLoop; gates the loop offsets (omit to leave unchanged).
//   @param time_scale - Constant playback time scale (double; must be non-zero -- ~0 is a FixedTime warp, not a play rate; omit to leave unchanged).
//   @returns { sequence_path: string, section_class: string, section_index: int, start_frame_offset: int, end_frame_offset: int, first_loop_start_frame_offset: int, hierarchical_bias: int, can_loop: bool, time_scale: double|null, set: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetSubSectionParameters(const TSharedPtr<FJsonObject>& Params)
{
    // Resolve the master sub/shot track. Cinematic shot track is-a sub track only at
    // the SECTION level (UMovieSceneCinematicShotSection IS-A UMovieSceneSubSection),
    // but the TRACK classes are distinct, so select by track_type.
    FString TrackTypeRaw;
    Params->TryGetStringField(TEXT("track_type"), TrackTypeRaw);
    const FString TrackTypeLower = TrackTypeRaw.ToLower();
    UClass* TrackClass = UMovieSceneSubTrack::StaticClass();
    if (TrackTypeLower == TEXT("shot") || TrackTypeLower == TEXT("cinematicshot") || TrackTypeLower == TEXT("cinematic_shot"))
    {
        TrackClass = UMovieSceneCinematicShotTrack::StaticClass();
    }

    // Exact-class match: UMovieSceneCinematicShotTrack IS-A UMovieSceneSubTrack, so a
    // plain "subsequence" lookup must NOT grab a shot track (and a "shot" lookup must
    // get the shot track exactly). Resolve precisely by GetClass()==TrackClass.
    ULevelSequence* Sequence = nullptr;
    UMovieSceneSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ResolveMasterSectionFromParams(Params, TrackClass, /*bExactClass*/ true, Sequence, Section)) { return Failure; }

    UMovieSceneSubSection* SubSection = nullptr;
    if (TSharedPtr<FJsonObject> Mismatch = UEAgentSequencerImpl::CastSection(Section, TEXT("UMovieSceneSubSection"), SubSection)) { return Mismatch; }

    int32 SectionIndex = 0;
    Params->TryGetNumberField(TEXT("section_index"), SectionIndex);

    // Validate time_scale (if provided) BEFORE mutating -- reject near-zero, which
    // FMovieSceneTimeWarpVariant::Set() silently turns into a FixedTime warp.
    bool bHasTimeScale = Params->HasField(TEXT("time_scale"));
    double TimeScale = 1.0;
    if (bHasTimeScale)
    {
        Params->TryGetNumberField(TEXT("time_scale"), TimeScale);
        if (FMath::IsNearlyZero(TimeScale))
        {
            return mcp_common::MakeErrorReply(
                TEXT("time_scale must be non-zero (a near-zero value is a FixedTime warp, not a play rate)"));
        }
    }

    const FScopedTransaction Transaction(LOCTEXT("SetSubSectionParameters", "MCP: Set Sub Section Parameters"));
    SubSection->Modify();

    FMovieSceneSectionParameters& P = SubSection->Parameters;

    // bCanLoop first -- it gates whether FirstLoopStartFrameOffset is meaningful.
    bool bCanLoop = false;
    if (Params->TryGetBoolField(TEXT("can_loop"), bCanLoop)) { P.bCanLoop = bCanLoop; }

    double D = 0.0;
    if (Params->TryGetNumberField(TEXT("start_frame_offset"), D))            { P.StartFrameOffset = FFrameNumber(static_cast<int32>(FMath::RoundToInt(D))); }
    if (Params->TryGetNumberField(TEXT("end_frame_offset"), D))              { P.EndFrameOffset = FFrameNumber(static_cast<int32>(FMath::RoundToInt(D))); }
    if (Params->TryGetNumberField(TEXT("first_loop_start_frame_offset"), D)) { P.FirstLoopStartFrameOffset = FFrameNumber(static_cast<int32>(FMath::RoundToInt(D))); }

    int32 Bias = 0;
    if (Params->TryGetNumberField(TEXT("hierarchical_bias"), Bias)) { P.HierarchicalBias = Bias; }

    if (bHasTimeScale)
    {
        // FMovieSceneTimeWarpVariant::Set(double) -- MOVIESCENE_API, headless-safe.
        P.TimeScale.Set(TimeScale);
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetStringField(TEXT("section_class"), SubSection->GetClass()->GetName());
    Response->SetNumberField(TEXT("section_index"), SectionIndex);
    Response->SetNumberField(TEXT("start_frame_offset"), P.StartFrameOffset.Value);
    Response->SetNumberField(TEXT("end_frame_offset"), P.EndFrameOffset.Value);
    Response->SetNumberField(TEXT("first_loop_start_frame_offset"), P.FirstLoopStartFrameOffset.Value);
    Response->SetNumberField(TEXT("hierarchical_bias"), P.HierarchicalBias);
    Response->SetBoolField(TEXT("can_loop"), P.bCanLoop);
    WriteTimeWarpReadback(Response, TEXT("time_scale"), P.TimeScale);
    Response->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_sub_section_parameters"));
}

// MCP:set_audio_section_properties - Set playback properties on an audio section (UMovieSceneAudioSection).  Optionally swaps the sound and toggles loop/play-until-finished/subtitle/attenuation flags. SetAttenuationSettings is a no-op at eval unless override_attenuation is true, so the override flag is applied first.
//   @param sequence_path - Full /Game path OR bare asset name of the Level Sequence that owns the (root) audio track.
//   @param sequence_name - Alias for sequence_path.
//   @param section_index - Index of the section on the resolved root audio track (default 0).
//   @param sound_path - Optional content path of a USoundBase to set as the section's sound.
//   @param looping [bool] - Section repeating/looping (maps to SetRepeating, the engine's loop knob; omit to leave unchanged).
//   @param play_until_finished [bool] - SetPlayUntilFinished (omit to leave unchanged).
//   @param suppress_subtitles [bool] - SetSuppressSubtitles (omit to leave unchanged).
//   @param override_attenuation [bool] - SetOverrideAttenuation; must be true for attenuation_path to take effect (omit to leave unchanged).
//   @param attenuation_path - Optional content path of a USoundAttenuation to set (only effective when override_attenuation is true).
//   @returns { sequence_path: string, section_class: string, section_index: int, sound: string, sound_set: bool, looping: bool, play_until_finished: bool, suppress_subtitles: bool, override_attenuation: bool, attenuation: string, attenuation_set: bool, set: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetAudioSectionProperties(const TSharedPtr<FJsonObject>& Params)
{
    ULevelSequence* Sequence = nullptr;
    UMovieSceneSection* Section = nullptr;
    // IsA match (bExactClass=false): there is a single audio-track hierarchy here.
    if (TSharedPtr<FJsonObject> Failure = ResolveMasterSectionFromParams(
        Params, UMovieSceneAudioTrack::StaticClass(), /*bExactClass*/ false, Sequence, Section))
    {
        return Failure;
    }

    UMovieSceneAudioSection* Audio = nullptr;
    if (TSharedPtr<FJsonObject> Mismatch = UEAgentSequencerImpl::CastSection(Section, TEXT("UMovieSceneAudioSection"), Audio)) { return Mismatch; }

    int32 SectionIndex = 0;
    Params->TryGetNumberField(TEXT("section_index"), SectionIndex);

    // Resolve the optional sound up front so a bad path is a clean error, not a silent skip.
    USoundBase* NewSound = nullptr;
    FString SoundPath;
    const bool bWantSound = Params->TryGetStringField(TEXT("sound_path"), SoundPath) && !SoundPath.IsEmpty();
    if (bWantSound)
    {
        NewSound = LoadObject<USoundBase>(nullptr, *SoundPath);
        if (!NewSound)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Failed to load USoundBase '%s'"), *SoundPath));
        }
    }

    USoundAttenuation* NewAtten = nullptr;
    FString AttenPath;
    const bool bWantAtten = Params->TryGetStringField(TEXT("attenuation_path"), AttenPath) && !AttenPath.IsEmpty();
    if (bWantAtten)
    {
        NewAtten = LoadObject<USoundAttenuation>(nullptr, *AttenPath);
        if (!NewAtten)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Failed to load USoundAttenuation '%s'"), *AttenPath));
        }
    }

    const FScopedTransaction Transaction(LOCTEXT("SetAudioSectionProperties", "MCP: Set Audio Section Properties"));
    Audio->Modify();

    bool bSoundSet = false;
    if (bWantSound) { Audio->SetSound(NewSound); bSoundSet = true; }

    bool bFlag = false;
    if (Params->TryGetBoolField(TEXT("looping"), bFlag))             { Audio->SetRepeating(bFlag); }
    if (Params->TryGetBoolField(TEXT("play_until_finished"), bFlag)) { Audio->SetPlayUntilFinished(bFlag); }
    if (Params->TryGetBoolField(TEXT("suppress_subtitles"), bFlag))  { Audio->SetSuppressSubtitles(bFlag); }

    // Override flag BEFORE the attenuation asset -- SetAttenuationSettings is gated by
    // bOverrideAttenuation at evaluation (UPROPERTY EditCondition).
    if (Params->TryGetBoolField(TEXT("override_attenuation"), bFlag)) { Audio->SetOverrideAttenuation(bFlag); }

    bool bAttenSet = false;
    if (bWantAtten) { Audio->SetAttenuationSettings(NewAtten); bAttenSet = true; }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetStringField(TEXT("section_class"), Audio->GetClass()->GetName());
    Response->SetNumberField(TEXT("section_index"), SectionIndex);
    Response->SetStringField(TEXT("sound"), Audio->GetSound() ? Audio->GetSound()->GetPathName() : TEXT(""));
    Response->SetBoolField(TEXT("sound_set"), bSoundSet);
    // IsRepeating() is the engine's own "is this section looping" query; it reads
    // bRepeating when the Sequencer.Audio.UseRepeating CVar is on, else bLooping.
    Response->SetBoolField(TEXT("looping"), Audio->IsRepeating());
    Response->SetBoolField(TEXT("play_until_finished"), Audio->GetPlayUntilFinished());
    Response->SetBoolField(TEXT("suppress_subtitles"), Audio->GetSuppressSubtitles());
    Response->SetBoolField(TEXT("override_attenuation"), Audio->GetOverrideAttenuation());
    Response->SetStringField(TEXT("attenuation"), Audio->GetAttenuationSettings() ? Audio->GetAttenuationSettings()->GetPathName() : TEXT(""));
    Response->SetBoolField(TEXT("attenuation_set"), bAttenSet);
    Response->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_audio_section_properties"));
}

// MCP:get_audio_section_properties - Read the playback properties of an audio section (UMovieSceneAudioSection), resolved like set_audio_section_properties (root audio track + section_index).  Pure reader (no transaction/save).  looping reads IsRepeating() (the engine's loop query); volume/pitch are sampled at frame 0; volume_key_count/pitch_key_count report the animated-channel key counts.
//   @param sequence_path - Full /Game path OR bare asset name of the Level Sequence that owns the (root) audio track.
//   @param sequence_name - Alias for sequence_path.
//   @param section_index - Index of the section on the resolved root audio track (default 0).
//   @returns { sequence_path: string, section_class: string, section_index: int, sound_path: string, start_offset: int, volume: number, pitch: number, volume_key_count: int, pitch_key_count: int, looping: bool, play_until_finished: bool, suppress_subtitles: bool, override_attenuation: bool, attenuation_path: string }
TSharedPtr<FJsonObject> HandleGetAudioSectionProperties(const TSharedPtr<FJsonObject>& Params)
{
    ULevelSequence* Sequence = nullptr;
    UMovieSceneSection* Section = nullptr;
    // IsA match (bExactClass=false): there is a single audio-track hierarchy here.
    if (TSharedPtr<FJsonObject> Failure = ResolveMasterSectionFromParams(
        Params, UMovieSceneAudioTrack::StaticClass(), /*bExactClass*/ false, Sequence, Section))
    {
        return Failure;
    }

    UMovieSceneAudioSection* Audio = nullptr;
    if (TSharedPtr<FJsonObject> Mismatch = UEAgentSequencerImpl::CastSection(Section, TEXT("UMovieSceneAudioSection"), Audio)) { return Mismatch; }

    int32 SectionIndex = 0;
    Params->TryGetNumberField(TEXT("section_index"), SectionIndex);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetStringField(TEXT("section_class"), Section->GetClass()->GetName());
    Response->SetNumberField(TEXT("section_index"), SectionIndex);
    Response->SetStringField(TEXT("sound_path"), Audio->GetSound() ? Audio->GetSound()->GetPathName() : TEXT(""));
    Response->SetNumberField(TEXT("start_offset"), Audio->GetStartOffset().Value);
    Response->SetNumberField(TEXT("volume"), Audio->GetSoundVolume(FFrameTime(0)));
    Response->SetNumberField(TEXT("pitch"), Audio->GetPitchMultiplier(FFrameTime(0)));
    Response->SetNumberField(TEXT("volume_key_count"), Audio->GetSoundVolumeChannel().GetData().GetTimes().Num());
    Response->SetNumberField(TEXT("pitch_key_count"), Audio->GetPitchMultiplierChannel().GetData().GetTimes().Num());
    // IsRepeating() is the engine's own "is this section looping" query.
    Response->SetBoolField(TEXT("looping"), Audio->IsRepeating());
    Response->SetBoolField(TEXT("play_until_finished"), Audio->GetPlayUntilFinished());
    Response->SetBoolField(TEXT("suppress_subtitles"), Audio->GetSuppressSubtitles());
    Response->SetBoolField(TEXT("override_attenuation"), Audio->GetOverrideAttenuation());
    Response->SetStringField(TEXT("attenuation_path"), Audio->GetAttenuationSettings() ? Audio->GetAttenuationSettings()->GetPathName() : TEXT(""));
    return mcp_common::MakeOkReply(Response);
}

// MCP:set_transform_section_mask - Set the evaluation channel mask on a 3D transform section (UMovieScene3DTransformSection::SetMask).  The mask is a visibility/eval filter built from EMovieSceneTransformChannel; it nulls the channel proxy (intended rebuild) and does NOT delete existing keys.
//   @param sequence_path - Full /Game path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param binding_guid - GUID of the binding whose transform section to edit.
//   @param track_type - Optional class filter (default resolves the transform track; see set_section_easing).
//   @param track_index - Index into the binding's tracks (default 0).
//   @param section_index - Index of the section on the resolved track (default 0).
//   @param channels - Channels to ENABLE: a JSON array (e.g. ["TranslationX","Rotation"]) OR a comma-separated string. Tokens: TranslationX/Y/Z, Translation, RotationX/Y/Z (Roll/Pitch/Yaw), Rotation, ScaleX/Y/Z, Scale, Weight, AllTransform, All, None.
//   @param binding_name_or_guid - Alias for binding_guid, read only when binding_guid is absent; it must also be a GUID string, a bound object's name is not resolved.
//   @returns { sequence_path: string, section_index: int, channels: string[], channel_mask: int, set: bool, saved: bool, note: string }
TSharedPtr<FJsonObject> HandleSetTransformSectionMask(const TSharedPtr<FJsonObject>& Params)
{
    ULevelSequence* Sequence = nullptr;
    UMovieSceneSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ResolveSectionFromParams(Params, Sequence, Section)) { return Failure; }

    UMovieScene3DTransformSection* Xform = nullptr;
    if (TSharedPtr<FJsonObject> Mismatch = UEAgentSequencerImpl::CastSection(Section, TEXT("UMovieScene3DTransformSection"), Xform)) { return Mismatch; }

    if (!Params->HasField(TEXT("channels")))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: channels (array or comma-separated string)"));
    }

    TArray<FString> Bad;
    const EMovieSceneTransformChannel Channels = ParseTransformChannels(Params, Bad);
    if (Bad.Num() > 0)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unrecognised transform channel(s): %s"), *FString::Join(Bad, TEXT(", "))));
    }

    int32 SectionIndex = 0;
    Params->TryGetNumberField(TEXT("section_index"), SectionIndex);

    const FScopedTransaction Transaction(LOCTEXT("SetTransformSectionMask", "MCP: Set Transform Section Mask"));
    Xform->Modify();
    // SetMask(FMovieSceneTransformMask) -- MOVIESCENETRACKS_API. Built from the OR'd
    // EMovieSceneTransformChannel via the mask's (EMovieSceneTransformChannel) ctor.
    Xform->SetMask(FMovieSceneTransformMask(Channels));

    const bool bSaved = SaveSequencePackage(Sequence);

    const EMovieSceneTransformChannel Applied = Xform->GetMask().GetChannels();

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetNumberField(TEXT("section_index"), SectionIndex);
    Response->SetArrayField(TEXT("channels"), TransformChannelsToJsonArray(Applied));
    Response->SetNumberField(TEXT("channel_mask"), static_cast<int32>(Applied));
    Response->SetBoolField(TEXT("set"), true);
    Response->SetStringField(TEXT("note"), TEXT("SetMask rebuilds the channel proxy and is an eval/visibility filter; existing keys are preserved."));
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_transform_section_mask"));
}

// MCP:set_transform_section_quaternion_interp - Turns quaternion (slerp) rotation interpolation on or off for one 3D transform section of a Level Sequence binding, in an undo transaction, and saves the sequence; keys are kept. The section is section_index on the binding's first track of track_type, or on its track_index-th track when track_type is omitted or unrecognised; a section that is not a 3D transform section is an error. For the section's blend type use set_transform_track_interp_mode; to enable or disable channels use set_transform_section_mask.
//   @param sequence_path - Full /Game path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param binding_guid - GUID of the binding whose transform section to edit.
//   @param track_type - Optional track kind, e.g. "transform": picks the binding's first track of that kind. Omitted or unrecognised, track_index picks the track instead.
//   @param track_index - Index into the binding's tracks (default 0).
//   @param section_index - Index of the section on the resolved track (default 0).
//   @param use_quaternion [bool] - Desired quaternion-interpolation state.
//   @param binding_name_or_guid - Alias for binding_guid, read only when binding_guid is absent; it must also be a GUID string, a bound object's name is not resolved.
//   @returns { sequence_path: string, section_index: int, use_quaternion_interpolation: bool, set: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetTransformSectionQuaternionInterp(const TSharedPtr<FJsonObject>& Params)
{
    ULevelSequence* Sequence = nullptr;
    UMovieSceneSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ResolveSectionFromParams(Params, Sequence, Section)) { return Failure; }

    UMovieScene3DTransformSection* Xform = nullptr;
    if (TSharedPtr<FJsonObject> Mismatch = UEAgentSequencerImpl::CastSection(Section, TEXT("UMovieScene3DTransformSection"), Xform)) { return Mismatch; }

    bool bUseQuat = false;
    if (!Params->TryGetBoolField(TEXT("use_quaternion"), bUseQuat))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: use_quaternion (bool)"));
    }

    int32 SectionIndex = 0;
    Params->TryGetNumberField(TEXT("section_index"), SectionIndex);

    const FScopedTransaction Transaction(LOCTEXT("SetTransformSectionQuatInterp", "MCP: Set Transform Section Quaternion Interp"));
    Xform->Modify();
    Xform->SetUseQuaternionInterpolation(bUseQuat);

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetNumberField(TEXT("section_index"), SectionIndex);
    Response->SetBoolField(TEXT("use_quaternion_interpolation"), Xform->GetUseQuaternionInterpolation());
    Response->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_transform_section_quaternion_interp"));
}

// MCP:set_skeletal_animation_section_params - Set the playback params on a skeletal-animation section (UMovieSceneSkeletalAnimationSection::Params).  Writes the public FMovieSceneSkeletalAnimationParams fields directly.
//   @param sequence_path - Full /Game path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param binding_guid - GUID of the binding whose skeletal-animation section to edit.
//   @param track_type - Optional class filter (default resolves the skeletal-animation track; see set_section_easing).
//   @param track_index - Index into the binding's tracks (default 0).
//   @param section_index - Index of the section on the resolved track (default 0).
//   @param reverse [bool] - bReverse (omit to leave unchanged).
//   @param slot_name - SlotName (FName; omit to leave unchanged).
//   @param start_frame_offset - StartFrameOffset in tick frames (omit to leave unchanged).
//   @param end_frame_offset - EndFrameOffset in tick frames (omit to leave unchanged).
//   @param skip_anim_notifiers [bool] - bSkipAnimNotifiers (omit to leave unchanged).
//   @param force_custom_mode [bool] - bForceCustomMode (omit to leave unchanged).
//   @param play_rate - Constant playback rate (double; must be non-zero -- ~0 is a FixedTime warp, not a play rate; omit to leave unchanged).
//   @param binding_name_or_guid - Alias for binding_guid, read only when binding_guid is absent; it must also be a GUID string, a bound object's name is not resolved.
//   @returns { sequence_path: string, section_index: int, reverse: bool, slot_name: string, start_frame_offset: int, end_frame_offset: int, skip_anim_notifiers: bool, force_custom_mode: bool, play_rate: double|null, set: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetSkeletalAnimationSectionParams(const TSharedPtr<FJsonObject>& Params)
{
    ULevelSequence* Sequence = nullptr;
    UMovieSceneSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ResolveSectionFromParams(Params, Sequence, Section)) { return Failure; }

    UMovieSceneSkeletalAnimationSection* Skel = nullptr;
    if (TSharedPtr<FJsonObject> Mismatch = UEAgentSequencerImpl::CastSection(Section, TEXT("UMovieSceneSkeletalAnimationSection"), Skel)) { return Mismatch; }

    int32 SectionIndex = 0;
    Params->TryGetNumberField(TEXT("section_index"), SectionIndex);

    // Validate play_rate (if provided) BEFORE mutating -- reject near-zero.
    const bool bHasPlayRate = Params->HasField(TEXT("play_rate"));
    double PlayRate = 1.0;
    if (bHasPlayRate)
    {
        Params->TryGetNumberField(TEXT("play_rate"), PlayRate);
        if (FMath::IsNearlyZero(PlayRate))
        {
            return mcp_common::MakeErrorReply(
                TEXT("play_rate must be non-zero (a near-zero value is a FixedTime warp, not a play rate)"));
        }
    }

    const FScopedTransaction Transaction(LOCTEXT("SetSkeletalAnimSectionParams", "MCP: Set Skeletal Animation Section Params"));
    Skel->Modify();

    FMovieSceneSkeletalAnimationParams& P = Skel->Params;

    bool bFlag = false;
    if (Params->TryGetBoolField(TEXT("reverse"), bFlag))             { P.bReverse = bFlag ? 1 : 0; }  // uint32:1 bitfield
    if (Params->TryGetBoolField(TEXT("skip_anim_notifiers"), bFlag)) { P.bSkipAnimNotifiers = bFlag; } // plain bool
    if (Params->TryGetBoolField(TEXT("force_custom_mode"), bFlag))   { P.bForceCustomMode = bFlag; }   // plain bool

    FString SlotName;
    if (Params->TryGetStringField(TEXT("slot_name"), SlotName) && !SlotName.IsEmpty()) { P.SlotName = FName(*SlotName); }

    double D = 0.0;
    if (Params->TryGetNumberField(TEXT("start_frame_offset"), D)) { P.StartFrameOffset = FFrameNumber(static_cast<int32>(FMath::RoundToInt(D))); }
    if (Params->TryGetNumberField(TEXT("end_frame_offset"), D))   { P.EndFrameOffset = FFrameNumber(static_cast<int32>(FMath::RoundToInt(D))); }

    if (bHasPlayRate)
    {
        // PlayRate is FMovieSceneTimeWarpVariant -- set via Set(double).
        P.PlayRate.Set(PlayRate);
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetNumberField(TEXT("section_index"), SectionIndex);
    Response->SetBoolField(TEXT("reverse"), P.bReverse != 0);
    Response->SetStringField(TEXT("slot_name"), P.SlotName.ToString());
    Response->SetNumberField(TEXT("start_frame_offset"), P.StartFrameOffset.Value);
    Response->SetNumberField(TEXT("end_frame_offset"), P.EndFrameOffset.Value);
    Response->SetBoolField(TEXT("skip_anim_notifiers"), P.bSkipAnimNotifiers);
    Response->SetBoolField(TEXT("force_custom_mode"), P.bForceCustomMode);
    WriteTimeWarpReadback(Response, TEXT("play_rate"), P.PlayRate);
    Response->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_skeletal_animation_section_params"));
}

// MCP:set_section_row_index - Set the row index of a sequencer section (UMovieSceneSection::SetRowIndex).  Only meaningful when the owning track supports multiple rows (reported as supports_multiple_rows); the owning track is also Modify()'d.
//   @param sequence_path - Full /Game path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param binding_guid - GUID of the binding whose section to edit.
//   @param track_type - Optional class filter (see set_section_easing).
//   @param track_index - Index into the binding's tracks (default 0).
//   @param section_index - Index of the section on the resolved track (default 0).
//   @param row_index - Desired row index (int, >= 0).
//   @param binding_name_or_guid - Alias for binding_guid, read only when binding_guid is absent; it must also be a GUID string, a bound object's name is not resolved.
//   @returns { sequence_path: string, section_index: int, row_index: int, supports_multiple_rows: bool, set: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetSectionRowIndex(const TSharedPtr<FJsonObject>& Params)
{
    ULevelSequence* Sequence = nullptr;
    UMovieSceneSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ResolveSectionFromParams(Params, Sequence, Section)) { return Failure; }

    int32 RowIndex = 0;
    if (!Params->TryGetNumberField(TEXT("row_index"), RowIndex))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: row_index (int)"));
    }
    if (RowIndex < 0)
    {
        return mcp_common::MakeErrorReply(TEXT("row_index must be >= 0"));
    }

    int32 SectionIndex = 0;
    Params->TryGetNumberField(TEXT("section_index"), SectionIndex);

    // RowIndex is a track-row concept -- Modify() the owning track too (not just the section).
    UMovieSceneTrack* OwningTrack = Section->GetTypedOuter<UMovieSceneTrack>();
    const bool bSupportsMultipleRows = OwningTrack ? OwningTrack->SupportsMultipleRows() : false;

    const FScopedTransaction Transaction(LOCTEXT("SetSectionRowIndex", "MCP: Set Section Row Index"));
    if (OwningTrack) { OwningTrack->Modify(); }
    Section->Modify();
    Section->SetRowIndex(RowIndex);

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetNumberField(TEXT("section_index"), SectionIndex);
    Response->SetNumberField(TEXT("row_index"), Section->GetRowIndex());
    Response->SetBoolField(TEXT("supports_multiple_rows"), bSupportsMultipleRows);
    Response->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_section_row_index"));
}

// MCP:set_section_color - Set the editor color tint of a sequencer section (UMovieSceneSection::SetColorTint).  The tint is WITH_EDITORONLY_DATA (a no-op in cooked/no-editor-data builds); in the editor it persists on the asset.
//   @param sequence_path - Full /Game path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param binding_guid - GUID of the binding whose section to edit.
//   @param track_type - Optional class filter (see set_section_easing).
//   @param track_index - Index into the binding's tracks (default 0).
//   @param section_index - Index of the section on the resolved track (default 0).
//   @param r - Red component 0-255 (int; default 0).
//   @param g - Green component 0-255 (int; default 0).
//   @param b - Blue component 0-255 (int; default 0).
//   @param a - Alpha component 0-255 (int; default 255).
//   @param binding_name_or_guid - Alias for binding_guid, read only when binding_guid is absent; it must also be a GUID string, a bound object's name is not resolved.
//   @returns { sequence_path: string, section_index: int, color: { r: int, g: int, b: int, a: int }, set: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetSectionColor(const TSharedPtr<FJsonObject>& Params)
{
    ULevelSequence* Sequence = nullptr;
    UMovieSceneSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ResolveSectionFromParams(Params, Sequence, Section)) { return Failure; }

    int32 SectionIndex = 0;
    Params->TryGetNumberField(TEXT("section_index"), SectionIndex);

    int32 R = 0, G = 0, B = 0, A = 255;
    Params->TryGetNumberField(TEXT("r"), R);
    Params->TryGetNumberField(TEXT("g"), G);
    Params->TryGetNumberField(TEXT("b"), B);
    Params->TryGetNumberField(TEXT("a"), A);
    const FColor Tint(
        (uint8)FMath::Clamp(R, 0, 255), (uint8)FMath::Clamp(G, 0, 255),
        (uint8)FMath::Clamp(B, 0, 255), (uint8)FMath::Clamp(A, 0, 255));

    const FScopedTransaction Transaction(LOCTEXT("SetSectionColor", "MCP: Set Section Color"));
    Section->Modify();
    // SetColorTint(const FColor&) -- MOVIESCENE_API; effect is WITH_EDITORONLY_DATA so
    // the call site needs no #if guard (declaration is always present), and in the
    // editor it writes ColorTint.
    Section->SetColorTint(Tint);

    const bool bSaved = SaveSequencePackage(Sequence);

    const FColor Applied = Section->GetColorTint();

    TSharedPtr<FJsonObject> Color = MakeShared<FJsonObject>();
    Color->SetNumberField(TEXT("r"), Applied.R);
    Color->SetNumberField(TEXT("g"), Applied.G);
    Color->SetNumberField(TEXT("b"), Applied.B);
    Color->SetNumberField(TEXT("a"), Applied.A);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetNumberField(TEXT("section_index"), SectionIndex);
    Response->SetObjectField(TEXT("color"), Color);
    Response->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_section_color"));
}

// ===========================================================================
// Channel-keyframe inspection + editing on a BINDING's section channels.
//
// Read/move/remove/interp on per-binding channels
// (set_keyframe_interpolation covers MASTER float tracks only):
//   * get_channel_keyframes              READER -- list every key on one channel.
//   * move_keyframe                      move one key to a new frame (idempotent via
//                                        MoveKey(bRemoveDuplicateKeys=true)).
//   * remove_keyframe                    remove one key (the locator is checked before
//                                        RemoveKey, whose check(IsValidIndex) is fatal).
//   * set_binding_channel_keyframe_interp edit one key's interp/tangent/value.
//
// All resolve the section via ResolveSectionFromParams (binding_guid +
// track_type + section_index), then a channel by channel_index across the section's
// channel proxy. A section carries exactly one channel value-kind:
//   transform track        -> FMovieSceneDoubleChannel (9: loc/rot/scale)
//   property/float/color   -> FMovieSceneFloatChannel
//   visibility/bool        -> FMovieSceneBoolChannel   (no interp/tangent)
// channel_type ("double"|"float"|"bool", default "double") selects which proxy view to
// index, matching the track the caller built. Persistence mirrors
// set_keyframe_interpolation: FScopedTransaction + Section->Modify() +
// SaveSequencePackage(Sequence). Reads use the const GetData() (no transaction/save).
// ===========================================================================

// MCP:get_channel_keyframes - Read every keyframe on one channel of a binding's section (resolved like set_section_easing).  Pure reader (no transaction/save).  channel_type selects which channel proxy view to index: "double" (transform loc/rot/scale, default), "float" (property/color), "bool" (visibility).  Bool keys omit interp/tangent fields.
//   @param sequence_path - Full /Game path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param binding_guid - GUID of the binding whose section to read.
//   @param track_type - Optional class filter (see set_section_easing) selecting the track on the binding.
//   @param track_index - Index into the binding's tracks (default 0; used when track_type omitted).
//   @param section_index - Index of the section on the resolved track (default 0).
//   @param channel_type - Channel value kind: "double" (default) | "float" | "bool".
//   @param channel_index [int] - Index of the channel within its proxy view (default 0).
//   @param binding_name_or_guid - Alias for binding_guid, read only when binding_guid is absent; it must also be a GUID string, a bound object's name is not resolved.
//   @returns { sequence_path: string, channel_index: int, channel_type: string, key_count: int, keyframes: [{ index: int, frame: int, time_seconds: number, value: number|bool, interp_mode: string, tangent_mode: string, arrive_tangent: number, leave_tangent: number }] }
TSharedPtr<FJsonObject> HandleGetChannelKeyframes(const TSharedPtr<FJsonObject>& Params)
{
    ULevelSequence* Sequence = nullptr;
    UMovieSceneSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ResolveSectionFromParams(Params, Sequence, Section)) { return Failure; }

    FString Error;
    ESeqChannelKind Kind;
    if (!SeqParseChannelKind(Params, Kind, Error)) { return mcp_common::MakeErrorReply(Error); }
    const int32 ChannelIndex = SeqReadChannelIndex(Params);

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    const FFrameRate TickResolution = MovieScene ? MovieScene->GetTickResolution() : FFrameRate(24000, 1);

    TArray<TSharedPtr<FJsonValue>> Keyframes;
    if (Kind == ESeqChannelKind::Double)
    {
        FMovieSceneDoubleChannel* Channel = SeqChannelAt<FMovieSceneDoubleChannel>(Section, ChannelIndex, TEXT("Double"), Error);
        if (!Channel) { return mcp_common::MakeErrorReply(Error); }
        Keyframes = SeqCurveKeysToJson(*Channel, TickResolution);
    }
    else if (Kind == ESeqChannelKind::Float)
    {
        FMovieSceneFloatChannel* Channel = SeqChannelAt<FMovieSceneFloatChannel>(Section, ChannelIndex, TEXT("Float"), Error);
        if (!Channel) { return mcp_common::MakeErrorReply(Error); }
        Keyframes = SeqCurveKeysToJson(*Channel, TickResolution);
    }
    else // Bool -- no interp/tangent.
    {
        FMovieSceneBoolChannel* Channel = SeqChannelAt<FMovieSceneBoolChannel>(Section, ChannelIndex, TEXT("Bool"), Error);
        if (!Channel) { return mcp_common::MakeErrorReply(Error); }
        const TMovieSceneChannelData<const bool> Data = Channel->GetData();
        const TArrayView<const FFrameNumber> Times = Data.GetTimes();
        const TArrayView<const bool> Values = Data.GetValues();
        for (int32 i = 0; i < Times.Num(); ++i)
        {
            TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>();
            K->SetNumberField(TEXT("index"), i);
            K->SetNumberField(TEXT("frame"), Times[i].Value);
            K->SetNumberField(TEXT("time_seconds"), TickResolution.AsSeconds(FFrameTime(Times[i])));
            K->SetBoolField(TEXT("value"), Values[i]);
            Keyframes.Add(MakeShared<FJsonValueObject>(K));
        }
    }

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetNumberField(TEXT("channel_index"), ChannelIndex);
    Response->SetStringField(TEXT("channel_type"), SeqChannelKindName(Kind));
    Response->SetNumberField(TEXT("key_count"), Keyframes.Num());
    Response->SetArrayField(TEXT("keyframes"), Keyframes);
    return mcp_common::MakeOkReply(Response);
}

// MCP:move_keyframe - Move one keyframe (located by 'index' or 'frame') to new_frame on a binding's section channel, preserving its value/interp/tangents.  Idempotent: MoveKey(bRemoveDuplicateKeys=true) collapses a duplicate when re-moved onto an existing frame.  channel_type selects the proxy view ("double" default | "float" | "bool").
//   @param sequence_path - Full /Game path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param binding_guid - GUID of the binding whose section to edit.
//   @param track_type - Optional class filter (see set_section_easing) selecting the track on the binding.
//   @param track_index - Index into the binding's tracks (default 0; used when track_type omitted).
//   @param section_index - Index of the section on the resolved track (default 0).
//   @param channel_type - Channel value kind: "double" (default) | "float" | "bool".
//   @param channel_index [int] - Index of the channel within its proxy view (default 0).
//   @param index [int] - Index of the key to move (mutually exclusive with frame).
//   @param frame [int] - Frame of the key to move (resolved via FindKey; provide index OR frame).
//   @param tolerance [int] - Optional FindKey frame tolerance when locating by frame (default 0).
//   @param new_frame [int!] - Destination frame number for the key.
//   @param binding_name_or_guid - Alias for binding_guid, read only when binding_guid is absent; it must also be a GUID string, a bound object's name is not resolved.
//   @returns { sequence_path: string, channel_index: int, channel_type: string, old_frame: int, new_frame: int, key_index: int, moved: bool, saved: bool }
TSharedPtr<FJsonObject> HandleMoveKeyframe(const TSharedPtr<FJsonObject>& Params)
{
    ULevelSequence* Sequence = nullptr;
    UMovieSceneSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ResolveSectionFromParams(Params, Sequence, Section)) { return Failure; }

    FString Error;
    ESeqChannelKind Kind;
    if (!SeqParseChannelKind(Params, Kind, Error)) { return mcp_common::MakeErrorReply(Error); }
    const int32 ChannelIndex = SeqReadChannelIndex(Params);

    double NewFrameD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("new_frame"), NewFrameD)) { return Missing; }
    const FFrameNumber NewFrame(static_cast<int32>(NewFrameD));

    const FScopedTransaction Transaction(LOCTEXT("MoveKeyframe", "MCP: Move Keyframe"));
    Section->Modify();

    int32 OldFrame = 0;
    int32 NewKeyIndex = INDEX_NONE;
    const TSharedPtr<FJsonObject> Failure = SeqEditKeyOfKind(Params, Section, Kind, ChannelIndex,
        [&](auto& Data, int32 KeyIndex) -> TSharedPtr<FJsonObject>
        {
            OldFrame = Data.GetTimes()[KeyIndex].Value;
            NewKeyIndex = Data.MoveKey(KeyIndex, NewFrame, /*bRemoveDuplicateKeys=*/true);
            return nullptr;
        });
    if (Failure.IsValid())
    {
        return Failure;
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetNumberField(TEXT("channel_index"), ChannelIndex);
    Response->SetStringField(TEXT("channel_type"), SeqChannelKindName(Kind));
    Response->SetNumberField(TEXT("old_frame"), OldFrame);
    Response->SetNumberField(TEXT("new_frame"), NewFrame.Value);
    Response->SetNumberField(TEXT("key_index"), NewKeyIndex);
    Response->SetBoolField(TEXT("moved"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("move_keyframe"));
}

// MCP:remove_keyframe - Remove one keyframe (located by 'index' or 'frame') from a binding's section channel (resolved like move_keyframe).  The out-of-range guard is applied BEFORE RemoveKey because TMovieSceneChannelData::RemoveKey has a fatal check(IsValidIndex) -- an invalid locator returns a clean error, never a crash.  channel_type selects the proxy view ("double" default | "float" | "bool").
//   @param sequence_path - Full /Game path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param binding_guid - GUID of the binding whose section to edit.
//   @param track_type - Optional class filter (see set_section_easing) selecting the track on the binding.
//   @param track_index - Index into the binding's tracks (default 0; used when track_type omitted).
//   @param section_index - Index of the section on the resolved track (default 0).
//   @param channel_type - Channel value kind: "double" (default) | "float" | "bool".
//   @param channel_index [int] - Index of the channel within its proxy view (default 0).
//   @param index [int] - Index of the key to remove (mutually exclusive with frame).
//   @param frame [int] - Frame of the key to remove (resolved via FindKey; provide index OR frame).
//   @param tolerance [int] - Optional FindKey frame tolerance when locating by frame (default 0).
//   @param binding_name_or_guid - Alias for binding_guid, read only when binding_guid is absent; it must also be a GUID string, a bound object's name is not resolved.
//   @returns { sequence_path: string, channel_type: string, channel_index: int, removed_frame: int, key_index: int, removed: bool, key_count_after: int, saved: bool }
TSharedPtr<FJsonObject> HandleRemoveKeyframe(const TSharedPtr<FJsonObject>& Params)
{
    ULevelSequence* Sequence = nullptr;
    UMovieSceneSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ResolveSectionFromParams(Params, Sequence, Section)) { return Failure; }

    FString Error;
    ESeqChannelKind Kind;
    if (!SeqParseChannelKind(Params, Kind, Error)) { return mcp_common::MakeErrorReply(Error); }
    const int32 ChannelIndex = SeqReadChannelIndex(Params);

    const FScopedTransaction Transaction(LOCTEXT("RemoveKeyframe", "MCP: Remove Keyframe"));
    Section->Modify();

    int32 RemovedFrame = 0;
    int32 KeyIndexOut = INDEX_NONE;
    int32 KeyCountAfter = 0;
    // SeqEditKeyOfKind rejects an unresolved key before RemoveKey, whose check(IsValidIndex) is fatal.
    const TSharedPtr<FJsonObject> Failure = SeqEditKeyOfKind(Params, Section, Kind, ChannelIndex,
        [&](auto& Data, int32 KeyIndex) -> TSharedPtr<FJsonObject>
        {
            RemovedFrame = Data.GetTimes()[KeyIndex].Value;
            Data.RemoveKey(KeyIndex);
            KeyIndexOut = KeyIndex;
            KeyCountAfter = Data.GetTimes().Num();
            return nullptr;
        });
    if (Failure.IsValid())
    {
        return Failure;
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetStringField(TEXT("channel_type"), SeqChannelKindName(Kind));
    Response->SetNumberField(TEXT("channel_index"), ChannelIndex);
    Response->SetNumberField(TEXT("removed_frame"), RemovedFrame);
    Response->SetNumberField(TEXT("key_index"), KeyIndexOut);
    Response->SetBoolField(TEXT("removed"), true);
    Response->SetNumberField(TEXT("key_count_after"), KeyCountAfter);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("remove_keyframe"));
}

// Shared edit body for a Double/Float key value struct (identical fields: Value, Tangent,
// InterpMode, TangentMode): the optional value param, then SeqApplyInterpEdit.
template <typename ValueType>
static bool SeqApplyKeyEdit(ValueType& Key, const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
    { double F = 0.0; if (Params->TryGetNumberField(TEXT("value"), F)) { Key.Value = static_cast<decltype(Key.Value)>(F); } }
    return SeqApplyInterpEdit(Params, Key, OutError);
}

// MCP:set_binding_channel_keyframe_interp - Set the interpolation mode, tangent mode, tangents and/or value of one keyframe on a binding's section channel (resolved like set_section_easing).  Like set_keyframe_interpolation but for per-binding channels: transform "double" (default) or property "float".  Bool channels have no interp/tangent and are rejected.  Idempotent (writing the same fields = same state).
//   @param sequence_path - Full /Game path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param binding_guid - GUID of the binding whose section to edit.
//   @param track_type - Optional class filter (see set_section_easing) selecting the track on the binding.
//   @param track_index - Index into the binding's tracks (default 0; used when track_type omitted).
//   @param section_index - Index of the section on the resolved track (default 0).
//   @param channel_type - Channel value kind: "double" (default) | "float". ("bool" is rejected -- no interp/tangent.)
//   @param channel_index [int] - Index of the channel within its proxy view (default 0).
//   @param index [int] - Index of the key to edit (mutually exclusive with frame).
//   @param frame [int] - Frame of the key to edit (resolved via FindKey; provide index OR frame).
//   @param tolerance [int] - Optional FindKey frame tolerance when locating by frame (default 0).
//   @param value [number] - Optional new key value (leaves unchanged if omitted).
//   @param interp_mode - Optional interpolation: "Cubic" | "Linear" | "Constant" (default leaves unchanged).
//   @param tangent_mode - Optional tangent mode (Cubic only): "Auto" | "User" | "Break" (default leaves unchanged).
//   @param arrive_tangent [number] - Optional arrive tangent value (Cubic).
//   @param leave_tangent [number] - Optional leave tangent value (Cubic).
//   @param binding_name_or_guid - Alias for binding_guid, read only when binding_guid is absent; it must also be a GUID string, a bound object's name is not resolved.
//   @returns { sequence_path: string, channel_index: int, channel_type: string, frame: int, key_index: int, value: number, interp_mode: string, tangent_mode: string, arrive_tangent: number, leave_tangent: number, set: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetBindingChannelKeyframeInterp(const TSharedPtr<FJsonObject>& Params)
{
    ULevelSequence* Sequence = nullptr;
    UMovieSceneSection* Section = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ResolveSectionFromParams(Params, Sequence, Section)) { return Failure; }

    FString Error;
    ESeqChannelKind Kind;
    if (!SeqParseChannelKind(Params, Kind, Error)) { return mcp_common::MakeErrorReply(Error); }
    if (Kind == ESeqChannelKind::Bool)
    {
        return mcp_common::MakeErrorReply(
            TEXT("channel_type 'bool' has no interpolation/tangent; use double or float"));
    }
    const int32 ChannelIndex = SeqReadChannelIndex(Params);

    const FScopedTransaction Transaction(LOCTEXT("SetBindingChannelKeyframeInterp", "MCP: Set Binding Channel Keyframe Interp"));
    Section->Modify();

    int32 KeyFrame = 0;
    int32 KeyIndexOut = INDEX_NONE;
    FString InterpOut, TangentOut;
    double ValueOut = 0.0;
    float ArriveOut = 0.0f, LeaveOut = 0.0f;

    const auto EditKey = [&](auto& Data, int32 KeyIndex) -> TSharedPtr<FJsonObject>
    {
        auto& Key = Data.GetValues()[KeyIndex];
        if (!SeqApplyKeyEdit(Key, Params, Error)) { return mcp_common::MakeErrorReply(Error); }
        KeyFrame = Data.GetTimes()[KeyIndex].Value;
        KeyIndexOut = KeyIndex;
        InterpOut = SeqInterpToStr(Key.InterpMode.GetValue());
        TangentOut = SeqTangentToStr(Key.TangentMode.GetValue());
        ValueOut = Key.Value;
        ArriveOut = Key.Tangent.ArriveTangent;
        LeaveOut = Key.Tangent.LeaveTangent;
        return nullptr;
    };
    const TSharedPtr<FJsonObject> Failure = (Kind == ESeqChannelKind::Double)
        ? SeqEditKey<FMovieSceneDoubleChannel>(Params, Section, ChannelIndex, TEXT("Double"), EditKey)
        : SeqEditKey<FMovieSceneFloatChannel>(Params, Section, ChannelIndex, TEXT("Float"), EditKey);
    if (Failure.IsValid())
    {
        return Failure;
    }

    const bool bSaved = SaveSequencePackage(Sequence);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetNumberField(TEXT("channel_index"), ChannelIndex);
    Response->SetStringField(TEXT("channel_type"), SeqChannelKindName(Kind));
    Response->SetNumberField(TEXT("frame"), KeyFrame);
    Response->SetNumberField(TEXT("key_index"), KeyIndexOut);
    Response->SetNumberField(TEXT("value"), ValueOut);
    Response->SetStringField(TEXT("interp_mode"), InterpOut);
    Response->SetStringField(TEXT("tangent_mode"), TangentOut);
    Response->SetNumberField(TEXT("arrive_tangent"), ArriveOut);
    Response->SetNumberField(TEXT("leave_tangent"), LeaveOut);
    Response->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("set_binding_channel_keyframe_interp"));
}

// ========================================================================== //
// Sequencer folder organization
//
// Folders are the Sequencer outliner's grouping primitive: a UMovieSceneFolder
// owns child folders, child tracks and child object-bindings. They are pure data
// on the UMovieScene asset (no open Sequencer, no PIE), so these commands are
// fully headless.
//
//   add_sequence_folder   create a root folder (or a child of a named parent),
//                         idempotent by NAME (UMovieScene::AddRootFolder only
//                         dedups by pointer identity, so the name scan is here).
//   remove_sequence_folder remove a root folder (or a named child folder),
//                         idempotent by NAME (already absent is a success).
//   move_track_to_folder  put a master track (resolved by class, optionally
//                         disambiguated by name) OR an object-binding (by GUID)
//                         into a named folder. The camera-cut track lives in a
//                         SEPARATE UMovieScene field (NOT in GetTracks()), so the
//                         track scan unions GetTracks() + GetCameraCutTrack().
//   get_sequence_folders  read the full root-folder tree recursively.
//
// All mutations: FScopedTransaction + MovieScene->Modify() + SaveSequencePackage().
// ========================================================================== //

// Find a root folder of the movie scene whose name matches (case-sensitive FName
// compare). Returns nullptr if none. (Root folders are name-deduped on creation.)
static UMovieSceneFolder* SeqFindRootFolderByName(UMovieScene* MovieScene, const FName& FolderName)
{
    if (!MovieScene)
    {
        return nullptr;
    }
    for (UMovieSceneFolder* Folder : MovieScene->GetRootFolders())
    {
        if (Folder && Folder->GetFolderName() == FolderName)
        {
            return Folder;
        }
    }
    return nullptr;
}

// Find a direct child folder of Parent by name. Returns nullptr if none.
static UMovieSceneFolder* SeqFindChildFolderByName(UMovieSceneFolder* Parent, const FName& FolderName)
{
    if (!Parent)
    {
        return nullptr;
    }
    for (UMovieSceneFolder* Child : Parent->GetChildFolders())
    {
        if (Child && Child->GetFolderName() == FolderName)
        {
            return Child;
        }
    }
    return nullptr;
}

// Depth-first search of the whole root-folder tree for the first folder matching
// the name. Used by move_track_to_folder so a nested folder is reachable by name.
static UMovieSceneFolder* SeqFindFolderRecursive(UMovieSceneFolder* Folder, const FName& FolderName)
{
    if (!Folder)
    {
        return nullptr;
    }
    if (Folder->GetFolderName() == FolderName)
    {
        return Folder;
    }
    for (UMovieSceneFolder* Child : Folder->GetChildFolders())
    {
        if (UMovieSceneFolder* Found = SeqFindFolderRecursive(Child, FolderName))
        {
            return Found;
        }
    }
    return nullptr;
}

static UMovieSceneFolder* SeqFindFolderInSceneRecursive(UMovieScene* MovieScene, const FName& FolderName)
{
    if (!MovieScene)
    {
        return nullptr;
    }
    for (UMovieSceneFolder* Root : MovieScene->GetRootFolders())
    {
        if (UMovieSceneFolder* Found = SeqFindFolderRecursive(Root, FolderName))
        {
            return Found;
        }
    }
    return nullptr;
}

// Serialize one folder (and, recursively, its subtree) to JSON. Read-only fields
// only: name + child_folders + child_tracks{class,name} + child_bindings[guid].
// Editor-only data (FolderColor / SortingOrder) is intentionally NOT exposed.
static TSharedPtr<FJsonObject> SeqFolderToJson(UMovieSceneFolder* Folder)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    if (!Folder)
    {
        return Obj;
    }
    Obj->SetStringField(TEXT("name"), Folder->GetFolderName().ToString());

    // child_tracks: {class, name}
    TArray<TSharedPtr<FJsonValue>> ChildTracks;
    for (UMovieSceneTrack* Track : Folder->GetChildTracks())
    {
        if (!Track)
        {
            continue;
        }
        TSharedPtr<FJsonObject> TObj = MakeShared<FJsonObject>();
        TObj->SetStringField(TEXT("class"), Track->GetClass()->GetName());
        TObj->SetStringField(TEXT("name"),
            Track->GetTrackName().IsNone() ? FString() : Track->GetTrackName().ToString());
        ChildTracks.Add(MakeShared<FJsonValueObject>(TObj));
    }
    Obj->SetArrayField(TEXT("child_tracks"), ChildTracks);

    // child_bindings: ["<guid>"]
    TArray<TSharedPtr<FJsonValue>> ChildBindings;
    for (const FGuid& Guid : Folder->GetChildObjectBindings())
    {
        ChildBindings.Add(MakeShared<FJsonValueString>(Guid.ToString()));
    }
    Obj->SetArrayField(TEXT("child_bindings"), ChildBindings);

    // child_folders: [ ...recursive... ]
    TArray<TSharedPtr<FJsonValue>> ChildFolders;
    for (UMovieSceneFolder* Child : Folder->GetChildFolders())
    {
        if (Child)
        {
            ChildFolders.Add(MakeShared<FJsonValueObject>(SeqFolderToJson(Child)));
        }
    }
    Obj->SetArrayField(TEXT("child_folders"), ChildFolders);

    return Obj;
}

// UMovieSceneFolder pitfalls for the folder commands: AddChildTrack does not dedup (so move_track_to_folder
// checks GetChildTracks() first), and GetChildFolders() returns a TArrayView by value, not a TArray
// reference.

// MCP:add_sequence_folder - Create a Sequencer outliner folder on a Level Sequence's movie scene (root folder, or a child of a named parent). Idempotent by name.
//   @param sequence_path - Asset path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param folder_name - Name of the folder to create.
//   @param parent_folder - Optional name of an existing parent folder; if given, the new folder is nested under it instead of at the root.
//   @returns { sequence_path: string, folder_name: string, parent_folder: string, created: bool, already_existed: bool, root_folder_count: int, saved: bool }

TSharedPtr<FJsonObject> HandleAddSequenceFolder(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequencePath(Params, SequencePath)) { return Missing; }

    FString FolderName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("folder_name"), FolderName)) { return Missing; }

    ULevelSequence* Sequence = LoadSequenceByPathOrName(SequencePath);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequencePath, MovieScene)) { return Failure; }

    const FName FolderFName(*FolderName);

    // Optional parent folder (by name). If requested, it must already exist.
    FString ParentName;
    Params->TryGetStringField(TEXT("parent_folder"), ParentName);
    UMovieSceneFolder* ParentFolder = nullptr;
    if (!ParentName.IsEmpty())
    {
        ParentFolder = SeqFindFolderInSceneRecursive(MovieScene, FName(*ParentName));
        if (!ParentFolder)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Parent folder '%s' not found"), *ParentName));
        }
    }

    // Idempotency: a folder with this name at the target level already exists.
    UMovieSceneFolder* Existing = ParentFolder
        ? SeqFindChildFolderByName(ParentFolder, FolderFName)
        : SeqFindRootFolderByName(MovieScene, FolderFName);

    bool bCreated = false;
    if (!Existing)
    {
        const FScopedTransaction Transaction(LOCTEXT("AddSequenceFolder", "MCP: Add Sequence Folder"));
        MovieScene->Modify();

        UMovieSceneFolder* NewFolder = NewObject<UMovieSceneFolder>(MovieScene, NAME_None, RF_Transactional);
        NewFolder->SetFolderName(FolderFName);
        if (ParentFolder)
        {
            ParentFolder->Modify();
            ParentFolder->AddChildFolder(NewFolder);
        }
        else
        {
            MovieScene->AddRootFolder(NewFolder);
        }
        bCreated = true;
    }

    const bool bSaved = bCreated ? SaveSequencePackage(Sequence) : false;

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetStringField(TEXT("folder_name"), FolderName);
    Response->SetStringField(TEXT("parent_folder"), ParentName);
    Response->SetBoolField(TEXT("created"), bCreated);
    Response->SetBoolField(TEXT("already_existed"), !bCreated);
    Response->SetNumberField(TEXT("root_folder_count"), MovieScene->GetRootFolders().Num());
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("add_sequence_folder"));
}

// Inverse of add_sequence_folder. Remove a root folder (POINTER
// overload UMovieScene::RemoveRootFolder, returns count) or a named child folder
// (UMovieSceneFolder::RemoveChildFolder, which self-Modifies). Idempotent by NAME:
// a folder that is already absent is a SUCCESS with already_absent:true (the exact
// inverse of add's already_existed) -- never an error. We only Modify/save when a
// folder was actually removed, so a no-op re-run touches nothing on disk.

// MCP:remove_sequence_folder - Remove a Sequencer outliner folder (root, or a named child of parent_folder) from a Level Sequence's movie scene. Inverse of add_sequence_folder; idempotent by name (already-absent is success, not error).
//   @param sequence_path - Asset path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param folder_name - Name of the folder to remove.
//   @param parent_folder - Optional name of the parent folder (searched recursively); if given, the child folder named folder_name is removed from it instead of a root folder.
//   @returns { sequence_path: string, folder_name: string, parent_folder: string, removed: bool, already_absent: bool, root_folder_count: int, saved: bool }

TSharedPtr<FJsonObject> HandleRemoveSequenceFolder(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequencePath(Params, SequencePath)) { return Missing; }

    FString FolderName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("folder_name"), FolderName)) { return Missing; }

    ULevelSequence* Sequence = LoadSequenceByPathOrName(SequencePath);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequencePath, MovieScene)) { return Failure; }

    const FName FolderFName(*FolderName);

    // Optional parent folder (by name, searched recursively). If requested, it must
    // already exist -- an absent parent is an honest error (mirror add's parent check).
    FString ParentName;
    Params->TryGetStringField(TEXT("parent_folder"), ParentName);
    UMovieSceneFolder* ParentFolder = nullptr;
    if (!ParentName.IsEmpty())
    {
        ParentFolder = SeqFindFolderInSceneRecursive(MovieScene, FName(*ParentName));
        if (!ParentFolder)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Parent folder '%s' not found"), *ParentName));
        }
    }

    // Resolve the folder to remove at the target level (child of parent, or root).
    UMovieSceneFolder* Target = ParentFolder
        ? SeqFindChildFolderByName(ParentFolder, FolderFName)
        : SeqFindRootFolderByName(MovieScene, FolderFName);

    UE_LOG(LogTemp, Verbose,
        TEXT("[remove_sequence_folder] seq='%s' folder='%s' parent='%s' found=%d root_folders_before=%d"),
        *Sequence->GetPathName(), *FolderName, *ParentName, Target ? 1 : 0,
        MovieScene->GetRootFolders().Num());

    bool bRemoved = false;
    if (Target)
    {
        const FScopedTransaction Transaction(LOCTEXT("RemoveSequenceFolder", "MCP: Remove Sequence Folder"));
        MovieScene->Modify();
        if (ParentFolder)
        {
            ParentFolder->Modify();
            ParentFolder->RemoveChildFolder(Target);
        }
        else
        {
            MovieScene->RemoveRootFolder(Target);
        }
        bRemoved = true;
    }

    const bool bSaved = bRemoved ? SaveSequencePackage(Sequence) : false;

    UE_LOG(LogTemp, Verbose,
        TEXT("[remove_sequence_folder] folder='%s' removed=%d already_absent=%d root_folders_after=%d saved=%d"),
        *FolderName, bRemoved ? 1 : 0, bRemoved ? 0 : 1,
        MovieScene->GetRootFolders().Num(), bSaved ? 1 : 0);

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetStringField(TEXT("folder_name"), FolderName);
    Response->SetStringField(TEXT("parent_folder"), ParentName);
    Response->SetBoolField(TEXT("removed"), bRemoved);
    Response->SetBoolField(TEXT("already_absent"), !bRemoved);
    Response->SetNumberField(TEXT("root_folder_count"), MovieScene->GetRootFolders().Num());
    return mcp_common::MakeSavedReply(Response, bSaved, TEXT("remove_sequence_folder"));
}

// MCP:move_track_to_folder - Move a master track (by class) or an object-binding (by GUID) into a named Sequencer folder. Idempotent.
//   @param sequence_path - Asset path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @param folder_name - Name of the destination folder (searched recursively).
//   @param track_class - Class name of the master track to move (e.g. "MovieSceneCameraCutTrack"). Mutually exclusive with binding_guid.
//   @param track_name - Optional track name to disambiguate when several tracks share the class.
//   @param binding_guid - GUID of an object-binding to move into the folder. Mutually exclusive with track_class.
//   @returns { sequence_path: string, folder_name: string, track_class: string, track_name: string, binding_guid: string, moved: bool, already_present: bool, saved: bool }

TSharedPtr<FJsonObject> HandleMoveTrackToFolder(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequencePath(Params, SequencePath)) { return Missing; }

    FString FolderName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("folder_name"), FolderName)) { return Missing; }

    // Exactly one of track_class / binding_guid identifies what to move.
    FString TrackClassName, BindingGuidStr;
    Params->TryGetStringField(TEXT("track_class"), TrackClassName);
    Params->TryGetStringField(TEXT("binding_guid"), BindingGuidStr);
    if (TrackClassName.IsEmpty() && BindingGuidStr.IsEmpty())
    {
        return mcp_common::MakeErrorReply(
            TEXT("Provide either track_class (master track) or binding_guid (object binding)"));
    }
    if (!TrackClassName.IsEmpty() && !BindingGuidStr.IsEmpty())
    {
        return mcp_common::MakeErrorReply(
            TEXT("track_class and binding_guid are mutually exclusive; provide only one"));
    }

    ULevelSequence* Sequence = LoadSequenceByPathOrName(SequencePath);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequencePath, MovieScene)) { return Failure; }

    // Resolve the destination folder (searched recursively so nested folders work).
    UMovieSceneFolder* Folder = SeqFindFolderInSceneRecursive(MovieScene, FName(*FolderName));
    if (!Folder)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Folder '%s' not found"), *FolderName));
    }

    // ---- Binding-guid path ------------------------------------------------ //
    if (!BindingGuidStr.IsEmpty())
    {
        FGuid BindingGuid;
        if (TSharedPtr<FJsonObject> Invalid = UEAgentSequencerImpl::ParseGuid(BindingGuidStr, TEXT("binding_guid"), BindingGuid)) { return Invalid; }
        // Verify the binding actually exists on the sequence (honest error otherwise).
        if (!MovieScene->FindBinding(BindingGuid))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("No binding with GUID '%s' on the sequence"), *BindingGuidStr));
        }

        const bool bAlready = Folder->GetChildObjectBindings().Contains(BindingGuid);
        bool bMoved = false;
        if (!bAlready)
        {
            const FScopedTransaction Transaction(LOCTEXT("MoveBindingToFolder", "MCP: Move Binding To Folder"));
            MovieScene->Modify();
            Folder->Modify();
            Folder->AddChildObjectBinding(BindingGuid);
            bMoved = true;
        }
        const bool bSaved = bMoved ? SaveSequencePackage(Sequence) : false;

        TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
        Response->SetStringField(TEXT("folder_name"), FolderName);
        Response->SetStringField(TEXT("track_class"), FString());
        Response->SetStringField(TEXT("track_name"), FString());
        Response->SetStringField(TEXT("binding_guid"), BindingGuid.ToString());
        Response->SetBoolField(TEXT("moved"), bMoved);
        Response->SetBoolField(TEXT("already_present"), bAlready);
        Response->SetBoolField(TEXT("saved"), bSaved);
        return mcp_common::MakeOkReply(Response);
    }

    // ---- Track-class path ------------------------------------------------- //
    // Optional name to disambiguate when several tracks share the class.
    FString TrackName;
    Params->TryGetStringField(TEXT("track_name"), TrackName);
    const bool bHaveName = !TrackName.IsEmpty();
    const FName TrackFName(*TrackName);

    // Build the candidate set. The camera-cut track is stored in a SEPARATE
    // UMovieScene field (CameraCutTrack), so it is NOT in GetTracks() -- union it
    // in explicitly so a request for "MovieSceneCameraCutTrack" actually resolves.
    TArray<UMovieSceneTrack*> Candidates;
    for (UMovieSceneTrack* Track : MovieScene->GetTracks())
    {
        if (Track && Track->GetClass()->GetName() == TrackClassName)
        {
            if (!bHaveName || Track->GetTrackName() == TrackFName)
            {
                Candidates.Add(Track);
            }
        }
    }
    if (UMovieSceneTrack* CamCut = MovieScene->GetCameraCutTrack())
    {
        if (CamCut->GetClass()->GetName() == TrackClassName)
        {
            if (!bHaveName || CamCut->GetTrackName() == TrackFName)
            {
                Candidates.Add(CamCut);
            }
        }
    }

    if (Candidates.Num() == 0)
    {
        return mcp_common::MakeErrorReply(
            bHaveName
                ? FString::Printf(TEXT("No track of class '%s' named '%s' found on the sequence"), *TrackClassName, *TrackName)
                : FString::Printf(TEXT("No track of class '%s' found on the sequence"), *TrackClassName));
    }
    if (Candidates.Num() > 1)
    {
        // Honest ambiguity: do not guess which track the caller meant.
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("%d tracks of class '%s' match%s; pass track_name to disambiguate"),
                Candidates.Num(), *TrackClassName,
                bHaveName ? TEXT(" the given name") : TEXT("")));
    }

    UMovieSceneTrack* Track = Candidates[0];
    const bool bAlready = Folder->GetChildTracks().Contains(Track);
    bool bMoved = false;
    if (!bAlready)
    {
        const FScopedTransaction Transaction(LOCTEXT("MoveTrackToFolder", "MCP: Move Track To Folder"));
        MovieScene->Modify();
        Folder->Modify();
        Folder->AddChildTrack(Track);
        bMoved = true;
    }
    const bool bSaved = bMoved ? SaveSequencePackage(Sequence) : false;

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetStringField(TEXT("folder_name"), FolderName);
    Response->SetStringField(TEXT("track_class"), TrackClassName);
    Response->SetStringField(TEXT("track_name"),
        Track->GetTrackName().IsNone() ? FString() : Track->GetTrackName().ToString());
    Response->SetStringField(TEXT("binding_guid"), FString());
    Response->SetBoolField(TEXT("moved"), bMoved);
    Response->SetBoolField(TEXT("already_present"), bAlready);
    Response->SetBoolField(TEXT("saved"), bSaved);
    return mcp_common::MakeOkReply(Response);
}

// MCP:get_sequence_folders - Read the Sequencer folder tree of a Level Sequence (root folders, recursively, with child tracks + bindings). Read-only.
//   @param sequence_path - Asset path OR bare asset name of the target Level Sequence.
//   @param sequence_name - Alias for sequence_path.
//   @returns { sequence_path: string, root_folder_count: int, folders: [{ name: string, child_folders: [...recursive...], child_tracks: [{class, name}], child_bindings: [guid_string] }] }

TSharedPtr<FJsonObject> HandleGetSequenceFolders(const TSharedPtr<FJsonObject>& Params)
{
    FString SequencePath;
    if (TSharedPtr<FJsonObject> Missing = UEAgentSequencerImpl::ReadSequencePath(Params, SequencePath)) { return Missing; }

    ULevelSequence* Sequence = LoadSequenceByPathOrName(SequencePath);
    UMovieScene* MovieScene = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentSequencerImpl::OpenMovieScene(Sequence, SequencePath, MovieScene)) { return Failure; }

    TArray<TSharedPtr<FJsonValue>> Folders;
    for (UMovieSceneFolder* Root : MovieScene->GetRootFolders())
    {
        if (Root)
        {
            Folders.Add(MakeShared<FJsonValueObject>(SeqFolderToJson(Root)));
        }
    }

    TSharedPtr<FJsonObject> Response = LevelSequenceUtils::NewPathPayload(Sequence);
    Response->SetNumberField(TEXT("root_folder_count"), Folders.Num());
    Response->SetArrayField(TEXT("folders"), Folders);
    return mcp_common::MakeOkReply(Response);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_attach_track"), &HandleAddAttachTrack },
    { TEXT("add_audio_section"), &HandleAddAudioSection },
    { TEXT("add_camera_cut"), &HandleAddCameraCut },
    { TEXT("add_camera_cut_section"), &HandleAddCameraCutSection },
    { TEXT("add_camera_cut_track"), &HandleAddCameraCutTrack },
    { TEXT("add_camera_focus_track"), &HandleAddCameraFocusTrack },
    { TEXT("add_component_material_track"), &HandleAddComponentMaterialTrack },
    { TEXT("add_event_track_section"), &HandleAddEventTrackSection },
    { TEXT("add_event_track_to_sequence"), &HandleAddEventTrackToSequence },
    { TEXT("add_fade_track_to_sequence"), &HandleAddFadeTrackToSequence },
    { TEXT("add_keyframe_to_property_track"), &HandleAddKeyframeToPropertyTrack },
    { TEXT("add_level_visibility_track"), &HandleAddLevelVisibilityTrack },
    { TEXT("add_marked_frame"), &HandleAddMarkedFrame },
    { TEXT("add_material_parameter_collection_track"), &HandleAddMaterialParameterCollectionTrack },
    { TEXT("add_property_track"), &HandleAddPropertyTrack },
    { TEXT("add_sequence_folder"), &HandleAddSequenceFolder },
    { TEXT("add_shot_section"), &HandleAddShotSection },
    { TEXT("add_shot_track"), &HandleAddShotTrack },
    { TEXT("add_skeletal_animation_track"), &HandleAddSkeletalAnimationTrack },
    { TEXT("add_slomo_track"), &HandleAddSlomoTrack },
    { TEXT("add_spawnable_to_sequence"), &HandleAddSpawnableToSequence },
    { TEXT("add_subcomponent_possessable"), &HandleAddSubcomponentPossessable },
    { TEXT("add_subsequence_track"), &HandleAddSubsequenceTrack },
    { TEXT("add_transform_keyframe"), &HandleAddTransformKeyframe },
    { TEXT("add_transform_track_to_binding"), &HandleAddTransformTrackToBinding },
    { TEXT("add_visibility_track"), &HandleAddVisibilityTrack },
    { TEXT("add_weighted_value_keyframe"), &HandleAddWeightedValueKeyframe },
    { TEXT("add_weighted_value_track"), &HandleAddWeightedValueTrack },
    { TEXT("bind_actor_property_to_sequence"), &HandleBindActorPropertyToSequence },
    { TEXT("bind_actor_to_sequence"), &HandleBindActorToSequence },
    { TEXT("bind_component_property_to_sequence"), &HandleBindComponentPropertyToSequence },
    { TEXT("bind_component_to_sequence"), &HandleBindComponentToSequence },
    { TEXT("clear_marked_frames"), &HandleClearMarkedFrames },
    { TEXT("create_cine_camera_in_sequence"), &HandleCreateCineCameraInSequence },
    { TEXT("duplicate_keyframe"), &HandleDuplicateKeyframe },
    { TEXT("get_audio_section_properties"), &HandleGetAudioSectionProperties },
    { TEXT("get_binding_tracks"), &HandleGetBindingTracks },
    { TEXT("get_channel_keyframes"), &HandleGetChannelKeyframes },
    { TEXT("get_marked_frames"), &HandleGetMarkedFrames },
    { TEXT("get_section_info"), &HandleGetSectionInfo },
    { TEXT("get_sequence_bindings"), &HandleGetSequenceBindings },
    { TEXT("get_sequence_folders"), &HandleGetSequenceFolders },
    { TEXT("get_sequence_master_tracks"), &HandleGetSequenceMasterTracks },
    { TEXT("get_sequence_possessables"), &HandleGetSequencePossessables },
    { TEXT("get_sequence_spawnables"), &HandleGetSequenceSpawnables },
    { TEXT("get_sequence_track_count"), &HandleGetSequenceTrackCount },
    { TEXT("move_keyframe"), &HandleMoveKeyframe },
    { TEXT("move_track_to_folder"), &HandleMoveTrackToFolder },
    { TEXT("remove_keyframe"), &HandleRemoveKeyframe },
    { TEXT("remove_marked_frame"), &HandleRemoveMarkedFrame },
    { TEXT("remove_section"), &HandleRemoveSection },
    { TEXT("remove_sequence_binding"), &HandleRemoveSequenceBinding },
    { TEXT("remove_sequence_folder"), &HandleRemoveSequenceFolder },
    { TEXT("remove_sequence_track"), &HandleRemoveSequenceTrack },
    { TEXT("set_attach_section_rules"), &HandleSetAttachSectionRules },
    { TEXT("set_audio_section_properties"), &HandleSetAudioSectionProperties },
    { TEXT("set_audio_section_start_offset"), &HandleSetAudioSectionStartOffset },
    { TEXT("set_audio_track_pitch"), &HandleSetAudioTrackPitch },
    { TEXT("set_audio_track_volume"), &HandleSetAudioTrackVolume },
    { TEXT("set_binding_channel_keyframe_interp"), &HandleSetBindingChannelKeyframeInterp },
    { TEXT("set_keyframe_interpolation"), &HandleSetKeyframeInterpolation },
    { TEXT("set_marked_frames_locked"), &HandleSetMarkedFramesLocked },
    { TEXT("set_playback_range_locked"), &HandleSetPlaybackRangeLocked },
    { TEXT("set_property_track_keyframe_count"), &HandleSetPropertyTrackKeyframeCount },
    { TEXT("set_section_active"), &HandleSetSectionActive },
    { TEXT("set_section_blend_type"), &HandleSetSectionBlendType },
    { TEXT("set_section_color"), &HandleSetSectionColor },
    { TEXT("set_section_completion_mode"), &HandleSetSectionCompletionMode },
    { TEXT("set_section_easing"), &HandleSetSectionEasing },
    { TEXT("set_section_locked"), &HandleSetSectionLocked },
    { TEXT("set_section_pre_post_rollout"), &HandleSetSectionPrePostRollout },
    { TEXT("set_section_range"), &HandleSetSectionRange },
    { TEXT("set_section_row_index"), &HandleSetSectionRowIndex },
    { TEXT("set_sequence_display_rate"), &HandleSetSequenceDisplayRate },
    { TEXT("set_sequence_playback_range"), &HandleSetSequencePlaybackRange },
    { TEXT("set_sequence_view_range"), &HandleSetSequenceViewRange },
    { TEXT("set_sequence_working_range"), &HandleSetSequenceWorkingRange },
    { TEXT("set_skeletal_animation_section_params"), &HandleSetSkeletalAnimationSectionParams },
    { TEXT("set_spawn_ownership"), &HandleSetSpawnOwnership },
    { TEXT("set_sub_section_parameters"), &HandleSetSubSectionParameters },
    { TEXT("set_transform_section_mask"), &HandleSetTransformSectionMask },
    { TEXT("set_transform_section_quaternion_interp"), &HandleSetTransformSectionQuaternionInterp },
    { TEXT("set_transform_track_interp_mode"), &HandleSetTransformTrackInterpMode },
};
} // namespace UEAgentSequencerCommands

#undef LOCTEXT_NAMESPACE
