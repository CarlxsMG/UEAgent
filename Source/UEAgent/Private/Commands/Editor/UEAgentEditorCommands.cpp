// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Asset/AssetCommandHelpers.h"  // MapPathTooLongForExternalPackages (save_level, load_level)
#include "Editor.h"
#include "EditorViewportClient.h"
#include "LevelEditorViewport.h"
#include "ImageUtils.h"
#include "HighResScreenshot.h"
#include "Engine/GameViewportClient.h"
#include "Misc/FileHelper.h"
#include "GameFramework/Actor.h"
#include "Engine/Selection.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Engine/RectLight.h"
#include "Engine/SkyLight.h"
#include "Components/LightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Camera/CameraActor.h"
#include "GameFramework/Volume.h"         // spawn_actor sends a volume to spawn_box_volume, which builds its brush
#include "Commands/Common/UEAgentGraphUtils.h" // spawn_actor: FUEAgentGraphUtils::ResolveClass for any actor class
#include "Mcp/McpCommandFamilies.h"
#include "GameFramework/Character.h"
#include "Components/StaticMeshComponent.h"
#include "EditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EditorAssetLibrary.h"
// Transaction support for undo/redo.
// Uses GEditor's standard transactions, not FChangeTransactor.
#include "ScopedTransaction.h"
// Level management (FEditorFileUtils)
#include "FileHelpers.h"
// Level operations. Engine APIs used:
//   ULevel::GetLevelScriptBlueprint(bDontCreate) -> ULevelScriptBlueprint* (ENGINE_API)
//   UEditorLevelUtils::MakeLevelCurrent(ULevel*, bEvenIfLocked) -> void (UNREALED_API)
//   UEditorLevelUtils::MoveActorsToLevel(TArray<AActor*>, ULevel*, ...) -> int32 (UNREALED_API)
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/LevelStreaming.h"
#include "Misc/PackageName.h"
#include "EditorLevelUtils.h"
// Ground traces (snap to ground)
#include "CollisionQueryParams.h"
// TActorIterator
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
// TObjectIterator<UWorld> for load_level's resident-world preflight (see the block
// comment above HandleLoadLevel). Mirrors the iterator the engine itself uses in
// UEditorEngine::CheckForWorldGCLeaks (EditorServer.cpp), which is the check that
// turns a leftover world into a Fatal error.
#include "UObject/UObjectIterator.h"
// IFileManager::FileExists / FileSize -- save_level derives its `success` from the .umap
// actually being on disk, never from the return value of the save call.
#include "HAL/FileManager.h"
// Class/object resolution: UClass::TryFindTypeSlow, FindFirstObject,
// MakeUniqueObjectName, StaticFindObjectFast, EFindFirstObjectOptions.
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
// Transaction commands. UTransactor (the base interface for
// GEditor->Trans) lives in Editor/Transactor.h; FTransaction (the per-record
// type) is in the same header. UTransBuffer (the concrete class behind the
// editor's trans buffer) is in Editor/TransBuffer.h.
#include "Editor/Transactor.h"
#include "Editor/TransBuffer.h"
// Landscape height sampling for snap_actor_to_landscape: headless line traces miss
// the landscape, so the heightmap is read directly (the sampler get_landscape_height_at uses).
#include "Landscape.h"                    // ALandscape
#include "LandscapeProxy.h"               // ALandscapeProxy / GetLandscapeInfo
#include "LandscapeInfo.h"                // ULandscapeInfo::GetLandscapeExtent
#include "Commands/Environment/LandscapeHeightSampling.h"  // LandscapeHeightSampling::SampleHeightAt
#include "Components/MeshComponent.h"     // UMeshComponent (mesh-bbox bottom, excludes shape volumes)
#include "Components/PrimitiveComponent.h"// UPrimitiveComponent::CalcBounds (local-space bbox)

/**
 * Editor-domain commands: actors in a level (listing, lookup, spawn, delete, transforms, selection,
 * cloning, grounding, alignment, instance components), the levels themselves (save, load, template map,
 * level Blueprint, current and sub-levels) and the editor's undo/redo transaction buffer.
 */
namespace UEAgentEditorCommands
{

// MCP-DOMAIN: Editor

// The transaction begin_transaction opened, or -1 when none is open.
static int32 ActiveTransactionIndex = -1;

namespace
{
    // Forward declarations -- definitions live further down in this translation
    // unit's (single) unnamed namespace, after the transaction handlers. Declared
    // up here so the snap / batch-terrain handlers above the definitions can call
    // the heightmap sampler that snap_actor_to_landscape uses.
    bool SampleFirstLandscapeHeightAtXY(UWorld* World, double WorldX, double WorldY,
                                        double& OutWorldZ, bool& OutInBounds,
                                        ALandscape*& OutLandscape, FString& OutReason);
    bool ComputeMeshBBoxBottomWorldZ(AActor* Actor, double& OutMinZ);

    // =====================================================================
    // Resolve the world for editor commands that accept a 'world' param, through
    // mcp_common::ResolveWorldSelector, the canonical resolver. A selector it does not
    // know is an ERROR, never the editor world answered with success: a read asked of the
    // server and answered by the editor writes itself up as "it does not replicate", and
    // sends someone to debug a system that works. (unknown_params cannot catch that:
    // `world` IS a known key -- what would be unknown is its VALUE.)
    //
    // On top of the shared resolver, a world that fails IsValidLowLevel() is rejected.
    // OutSelector receives the normalized selector, which these handlers echo in the
    // `world` field of their replies.
    //
    // DEFAULT: absent / 'editor' is the editor world, mcp_common::GetEditorWorld().
    //
    // On failure returns nullptr and fills OutError with WHAT was asked for and WHICH
    // worlds actually exist. That message is the point: an empty "not found" is what
    // makes a caller conclude the game is broken.
    // =====================================================================
    UWorld* ResolveEditorOrPieWorld(const TSharedPtr<FJsonObject>& Params,
                                    FString& OutSelector, FString& OutError)
    {
        UWorld* World = mcp_common::ResolveWorldSelector(Params, TEXT("editor"), OutSelector, OutError);

        // A world that fails IsValidLowLevel() is rejected rather than handed back.
        if (World && !World->IsValidLowLevel())
        {
            OutError = TEXT("No valid world found");
            return nullptr;
        }
        // Belt and braces: every nullptr path of the canonical resolver fills OutError,
        // but an empty error message would be exactly the silent failure to avoid.
        if (!World && OutError.IsEmpty())
        {
            OutError = FString::Printf(
                TEXT("No valid world found for world='%s'"), *OutSelector);
        }
        return World;
    }
}

// =====================================================================
// Plumbing shared by the actor, selection, level and component handlers
// below.
// =====================================================================
namespace UEAgentEditorCommandsImpl
{
    // The editor world, provided it exists and passes the low-level validity check.
    UWorld* ValidEditorWorld()
    {
        UWorld* Candidate = mcp_common::GetEditorWorld();
        if (Candidate == nullptr || !Candidate->IsValidLowLevel())
        {
            return nullptr;
        }
        return Candidate;
    }

    TSharedPtr<FJsonObject> NoWorldReply()
    {
        return mcp_common::MakeErrorReply(TEXT("No valid world found"));
    }

    TSharedPtr<FJsonObject> NoEditorReply()
    {
        return mcp_common::MakeErrorReply(TEXT("GEditor not available"));
    }

    TSharedPtr<FJsonObject> EmptyParamReply(const TCHAR* Field)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Parameter '%s' cannot be empty"), Field));
    }

    TSharedPtr<FJsonObject> ActorNotFoundReply(const FString& Name)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *Name));
    }

    // Caption of an undo-history entry: "<Verb>: <Subject>".
    FText UndoTitle(const TCHAR* Verb, const FString& Subject)
    {
        return FText::FromString(FString::Printf(TEXT("%s: %s"), Verb, *Subject));
    }

    // Every live actor of World, in actor-iterator order.
    TArray<AActor*> SnapshotActors(UWorld* World)
    {
        TArray<AActor*> Snapshot;
        UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), Snapshot);
        return Snapshot;
    }

    // First candidate whose INTERNAL object name equals Name. FString equality ignores
    // case, so this is a case-insensitive whole-string match; editor labels never count.
    AActor* PickByObjectName(const TArray<AActor*>& Candidates, const FString& Name)
    {
        for (AActor* Candidate : Candidates)
        {
            if (Candidate != nullptr && Candidate->GetName() == Name)
            {
                return Candidate;
            }
        }
        return nullptr;
    }

    // The editor world's actor whose internal name is Name; nullptr, with OutFailure set, when either is missing.
    AActor* ResolveEditorWorldActor(const FString& Name, UWorld*& OutWorld, TSharedPtr<FJsonObject>& OutFailure)
    {
        OutWorld = ValidEditorWorld();
        if (OutWorld == nullptr)
        {
            OutFailure = NoWorldReply();
            return nullptr;
        }
        AActor* Found = mcp_common::FindActorByObjectName(OutWorld, Name);
        if (Found == nullptr)
        {
            OutFailure = ActorNotFoundReply(Name);
        }
        return Found;
    }

    // Resolves the string entries of Names against Pool, each one to its first
    // internal-name match. Entries that are not strings, or are empty, are skipped
    // silently; a name without a match is appended to OutMisses when one is given.
    // A name listed twice resolves twice.
    TArray<AActor*> ResolveNameList(const TArray<TSharedPtr<FJsonValue>>& Names,
                                    const TArray<AActor*>& Pool, TArray<FString>* OutMisses)
    {
        TArray<AActor*> Resolved;
        for (const TSharedPtr<FJsonValue>& Entry : Names)
        {
            FString Wanted;
            if (!Entry->TryGetString(Wanted) || Wanted.IsEmpty())
            {
                continue;
            }
            if (AActor* Match = PickByObjectName(Pool, Wanted))
            {
                Resolved.Add(Match);
            }
            else if (OutMisses != nullptr)
            {
                OutMisses->Add(Wanted);
            }
        }
        return Resolved;
    }

    // Compact actor JSON (name, class, transform) for each actor, in order.
    TArray<TSharedPtr<FJsonValue>> ActorsToJson(const TArray<AActor*>& Actors)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Reserve(Actors.Num());
        for (AActor* Each : Actors)
        {
            Out.Add(mcp_common::DescribeActorValue(Each));
        }
        return Out;
    }

    // Editor selection plumbing. Callers have already made sure GEditor is set.
    void ClearEditorSelection()
    {
        GEditor->SelectNone(/*bNoteSelectionChange=*/false, /*bDeselectBSPSurfs=*/true, /*WarnAboutManyActors=*/false);
    }

    void AddToEditorSelection(AActor* Actor, bool bNotify)
    {
        GEditor->SelectActor(Actor, /*bInSelected=*/true, bNotify, /*bSelectEvenIfHidden=*/true);
    }

    // The actors of World held by Selection (the editor's actor selection), in selection
    // order. A null selection yields nothing.
    TArray<AActor*> SelectedActorsIn(USelection* Selection, UWorld* World)
    {
        TArray<AActor*> InWorld;
        if (Selection == nullptr)
        {
            return InWorld;
        }
        for (FSelectionIterator Cursor(*Selection); Cursor; ++Cursor)
        {
            AActor* AsActor = Cast<AActor>(*Cursor);
            if (AsActor != nullptr && AsActor->GetWorld() == World)
            {
                InWorld.Add(AsActor);
            }
        }
        return InWorld;
    }

    // Writes onto Transform whichever of location / rotation / scale Params carries;
    // a component that is absent keeps its current value. Used both for the request
    // itself and for the per-actor entries of a batch.
    void OverlayTransformFields(const TSharedPtr<FJsonObject>& Params, FTransform& Transform)
    {
        const bool bMove = Params->HasField(TEXT("location"));
        const bool bTurn = Params->HasField(TEXT("rotation"));
        const bool bResize = Params->HasField(TEXT("scale"));
        if (bMove)
        {
            Transform.SetLocation(mcp_common::ReadJsonVector(Params, TEXT("location")));
        }
        if (bTurn)
        {
            Transform.SetRotation(mcp_common::ReadJsonRotator(Params, TEXT("rotation")).Quaternion());
        }
        if (bResize)
        {
            Transform.SetScale3D(mcp_common::ReadJsonVector(Params, TEXT("scale")));
        }
    }

    // Visibility trace from From straight down by Depth, ignoring the actor being moved.
    bool TraceStraightDown(UWorld* World, const AActor* Ignored, const FVector& From, float Depth, FHitResult& OutHit)
    {
        FCollisionQueryParams Query;
        Query.AddIgnoredActor(Ignored);
        const FVector To = From - FVector(0, 0, Depth);
        return World->LineTraceSingleByChannel(OutHit, From, To, ECC_Visibility, Query);
    }

    // What align_actors copies onto every member.
    struct FAlignTarget
    {
        FVector Location;
        FRotator Rotation;
        FVector Scale;
    };

    FAlignTarget AlignTargetFrom(const AActor* Source)
    {
        const FTransform& Pose = Source->GetTransform();
        return FAlignTarget{ Pose.GetLocation(), Pose.GetRotation().Rotator(), Pose.GetScale3D() };
    }

    // Componentwise mean of the group; rotations are averaged as Euler angles.
    FAlignTarget AlignTargetAveraged(const TArray<AActor*>& Group)
    {
        FVector LocationSum = FVector::ZeroVector;
        FVector EulerSum = FVector::ZeroVector;
        FVector ScaleSum = FVector::ZeroVector;
        for (const AActor* Member : Group)
        {
            const FTransform& Pose = Member->GetTransform();
            LocationSum += Pose.GetLocation();
            EulerSum += Pose.GetRotation().Rotator().Euler();
            ScaleSum += Pose.GetScale3D();
        }
        const int32 Count = Group.Num();
        return FAlignTarget{ LocationSum / Count, FRotator::MakeFromEuler(EulerSum / Count), ScaleSum / Count };
    }

    // The engine actor classes spawn_actor accepts, by (case-insensitive) name; each light also by its short name.
    UClass* BuiltInSpawnClass(const FString& TypeName)
    {
        struct FSpawnable
        {
            const TCHAR* Name;
            UClass* Class;
        };
        const FSpawnable Spawnables[] = {
            { TEXT("StaticMeshActor"), AStaticMeshActor::StaticClass() },
            { TEXT("PointLight"), APointLight::StaticClass() },
            { TEXT("SpotLight"), ASpotLight::StaticClass() },
            { TEXT("DirectionalLight"), ADirectionalLight::StaticClass() },
            { TEXT("RectLight"), ARectLight::StaticClass() },
            { TEXT("SkyLight"), ASkyLight::StaticClass() },
            { TEXT("Point"), APointLight::StaticClass() },
            { TEXT("Spot"), ASpotLight::StaticClass() },
            { TEXT("Directional"), ADirectionalLight::StaticClass() },
            { TEXT("Rect"), ARectLight::StaticClass() },
            { TEXT("Sky"), ASkyLight::StaticClass() },
            { TEXT("CameraActor"), ACameraActor::StaticClass() },
            // A default ACharacter; gameplay characters derive their own classes.
            { TEXT("Character"), ACharacter::StaticClass() },
        };
        for (const FSpawnable& Row : Spawnables)
        {
            if (TypeName == Row.Name)
            {
                return Row.Class;
            }
        }
        return nullptr;
    }

    // A static mesh from a full object path or a bare package path. UEditorAssetLibrary
    // is not reliable headless for /Engine content (Asset Registry lag): LoadObject
    // takes the full object path, and FSoftObjectPath::TryLoad also resolves a bare
    // package path such as "/Engine/BasicShapes/Cube".
    UStaticMesh* LoadMeshForSpawn(const FString& MeshAsset)
    {
        if (UStaticMesh* Direct = LoadObject<UStaticMesh>(nullptr, *MeshAsset))
        {
            return Direct;
        }
        const FSoftObjectPath Soft(MeshAsset);
        return Soft.IsValid() ? Cast<UStaticMesh>(Soft.TryLoad()) : nullptr;
    }

    // The component of Owner whose object name equals Name (case-insensitive), or nullptr.
    UActorComponent* OwnedComponentNamed(const AActor* Owner, const FString& Name)
    {
        for (UActorComponent* Part : Owner->GetComponents())
        {
            if (Part != nullptr && Part->GetName() == Name)
            {
                return Part;
            }
        }
        return nullptr;
    }

    // Relative placement of a freshly added scene component, from the request.
    void ApplyRelativePlacement(const TSharedPtr<FJsonObject>& Params, USceneComponent* Scene)
    {
        const bool bOffset = Params->HasField(TEXT("location"));
        const bool bTilt = Params->HasField(TEXT("rotation"));
        const bool bResize = Params->HasField(TEXT("scale"));
        if (bOffset)
        {
            Scene->SetRelativeLocation(mcp_common::ReadJsonVector(Params, TEXT("location")));
        }
        if (bTilt)
        {
            Scene->SetRelativeRotation(mcp_common::ReadJsonRotator(Params, TEXT("rotation")));
        }
        if (bResize)
        {
            Scene->SetRelativeScale3D(mcp_common::ReadJsonVector(Params, TEXT("scale")));
        }
    }

    // Component class from the string a caller typed. There is no ANY_PACKAGE, and
    // FindObject<UClass>(nullptr, "StaticMeshComponent") does not find a bare name, so
    // the lookups run in this order: the string as given (short or path form, through
    // the slow type search), then with a "Component" suffix; a path is then loaded as
    // written, while a bare name is tried as /Script/<Module>.<Name> under a few core
    // modules. The caller still checks that the result is an actor component.
    UClass* ResolveComponentClass(const FString& Requested)
    {
        const auto FindType = [](const FString& Candidate) -> UClass*
        {
            if (Candidate.IsEmpty())
            {
                return nullptr;
            }
            // Short names and full path names alike; no short-name warning.
            return UClass::TryFindTypeSlow<UClass>(Candidate, EFindFirstObjectOptions::None);
        };
        // A /Script path is loaded first and searched as a type second.
        const auto FromScriptPath = [&FindType](const FString& ScriptPath) -> UClass*
        {
            UClass* Loaded = LoadObject<UClass>(nullptr, *ScriptPath);
            return Loaded != nullptr ? Loaded : FindType(ScriptPath);
        };

        if (UClass* AsGiven = FindType(Requested))
        {
            return AsGiven;
        }
        if (!Requested.EndsWith(TEXT("Component")))
        {
            if (UClass* WithSuffix = FindType(Requested + TEXT("Component")))
            {
                return WithSuffix;
            }
        }

        if (Requested.Contains(TEXT("/")))
        {
            // An explicit object or Blueprint class path.
            if (UClass* Direct = LoadObject<UClass>(nullptr, *Requested))
            {
                return Direct;
            }
            return LoadClass<UActorComponent>(nullptr, *Requested);
        }

        // A bare name: drop a leading "U" class prefix ("UStaticMeshComponent"), then
        // probe the script modules, each with and without the "Component" suffix.
        FString Bare = Requested;
        if (Bare.StartsWith(TEXT("U")) && Bare.Len() > 1 && FChar::IsUpper(Bare[1]))
        {
            Bare = Bare.RightChop(1);
        }
        const bool bBareHasSuffix = Bare.EndsWith(TEXT("Component"));
        static const TCHAR* const ScriptModules[] = { TEXT("Engine"), TEXT("CoreUObject"), TEXT("GameplayAbilities") };
        for (const TCHAR* Module : ScriptModules)
        {
            if (UClass* Plain = FromScriptPath(FString::Printf(TEXT("/Script/%s.%s"), Module, *Bare)))
            {
                return Plain;
            }
            if (!bBareHasSuffix)
            {
                if (UClass* Suffixed = FromScriptPath(FString::Printf(TEXT("/Script/%s.%sComponent"), Module, *Bare)))
                {
                    return Suffixed;
                }
            }
        }
        return nullptr;
    }

    // The actor's new world Z when grounding it at GroundZ: either its origin goes on the
    // ground ('actor_origin'), or the whole actor is shifted so its lowest mesh point
    // touches it ('mesh_bbox_bottom'). Without usable mesh geometry the origin is grounded
    // instead, and OutAlignUsed says so.
    double GroundedActorZ(AActor* Actor, double CurrentZ, double GroundZ, const FString& Align, FString& OutAlignUsed)
    {
        OutAlignUsed = Align;
        if (Align == TEXT("actor_origin"))
        {
            return GroundZ;
        }
        double LowestZ = 0.0;
        if (ComputeMeshBBoxBottomWorldZ(Actor, LowestZ))
        {
            return CurrentZ + (GroundZ - LowestZ);
        }
        OutAlignUsed = TEXT("actor_origin");
        return GroundZ;
    }

    // Moves Actor vertically to NewZ, keeping X / Y (taken from From) and the rest of its transform.
    void SetActorWorldZ(AActor* Actor, const FVector& From, double NewZ)
    {
        FTransform Pose = Actor->GetTransform();
        Pose.SetLocation(FVector(From.X, From.Y, NewZ));
        Actor->SetActorTransform(Pose);
    }
}

// Actors: census, lookup, spawn, removal, one-actor transform

// MCP:get_actors_in_level - Dump EVERY actor of a world (name, class, transform), capped by max_results at 5000, with total_actors_in_level and results_truncated so you know if you saw all of it. The unfiltered census: reach for get_actors_by_class when you know the class, find_actors_by_name for a name/label substring, get_actors_in_radius for a place. Its world selector is the canonical one: 'editor' (default), 'pie'/'game', 'server', 'client' and 'pie:N' all resolve for real, an unrecognised value FAILS naming the worlds that exist instead of quietly answering from the editor world, and every reply carries world_package/world_type/net_mode/pie_instance so you can see WHICH world answered rather than assuming.
//   @param max_results - Optional cap on returned actors (default 5000). Must be > 0. Truncation is reported, never silent.
//   @param world - Optional world selector: absent/'editor' (default) | 'pie' (ambiguous: PlayWorld, else the first PIE context) | 'game' (alias of 'pie') | 'server' (the PIE world with authority, GetNetMode() != NM_Client) | 'client' | 'pie:N' (the PIE context whose PIEInstance is N). Case-insensitive; an unrecognised value is a clean error listing the worlds present, never a silent fallback to the editor world.
//   @returns { actors: [actor JSON], actors_count: int, total_actors_in_level: int, results_truncated: bool, max_results: int, world: string (the normalized selector that resolved), world_name: string, world_package: string (in World Partition the UEDPIE_N prefix lives HERE, not in world_name), world_type: string, net_mode: string, pie_instance: int }

TSharedPtr<FJsonObject> HandleGetActorsInLevel(const TSharedPtr<FJsonObject>& Params)
{
    // Optional cap on how many actors are serialized. A fractional JSON number is
    // truncated toward zero.
    int32 Cap = 5000;
    double CapFromJson = 0.0;
    if (Params->TryGetNumberField(TEXT("max_results"), CapFromJson))
    {
        Cap = static_cast<int32>(CapFromJson);
    }
    if (Cap <= 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Parameter 'max_results' must be greater than 0"));
    }

    // The editor world unless `world` names another one. 'pie'/'game'/'server'/'client'/
    // 'pie:N' resolve for real, and an unknown selector FAILS (listing the worlds that do
    // exist) instead of quietly answering from the editor.
    FString SelectorUsed;
    FString ResolveFailure;
    UWorld* World = ResolveEditorOrPieWorld(Params, SelectorUsed, ResolveFailure);
    if (!World)
    {
        return mcp_common::MakeErrorReply(ResolveFailure);
    }

    const TArray<AActor*> Everything = UEAgentEditorCommandsImpl::SnapshotActors(World);
    TArray<TSharedPtr<FJsonValue>> Listed;
    for (AActor* Each : Everything)
    {
        if (Listed.Num() >= Cap)
        {
            break;
        }
        if (Each != nullptr)
        {
            Listed.Add(mcp_common::DescribeActorValue(Each));
        }
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetArrayField(TEXT("actors"), Listed);
    Reply->SetNumberField(TEXT("actors_count"), Listed.Num());
    Reply->SetNumberField(TEXT("total_actors_in_level"), Everything.Num());
    Reply->SetBoolField(TEXT("results_truncated"), Everything.Num() > Cap);
    Reply->SetNumberField(TEXT("max_results"), Cap);
    Reply->SetStringField(TEXT("world"), SelectorUsed);
    // The exact identity of the world that answered: world_name, world_package (where a
    // World Partition map keeps its UEDPIE_N prefix), world_type, net_mode, pie_instance.
    mcp_common::AddWorldEvidence(Reply, World);
    return Reply;
}

// MCP:find_actors_by_name - Locate actors by a SUBSTRING of their internal name and/or their editor label, returning name, label, class and transform for each hit. This is the one to use when you know what a thing is CALLED but not its class -- get_actors_by_class needs an exact class name, and this one alone can search labels (match='label'), which is where hand-assigned names live. Plain case-INSENSITIVE substring, no wildcards and no regex: 'turret' matches BP_Turret_C_2, and '*' matches literally nothing. Its world selector is the canonical one: 'editor' (default), 'pie'/'game', 'server', 'client' and 'pie:N' all resolve for real, an unrecognised value FAILS instead of falling through to the editor world, and the reply stamps world_package/world_type/net_mode/pie_instance so you can tell which world answered.
//   @pack actors
//   @param pattern - Substring to look for, case-insensitive, no wildcards or regex. Cannot be empty.
//   @param match - Optional 'name' | 'label' | 'both' (default 'both'). 'name' = internal object name only; 'label' = the editor label from the outliner; 'both' = a hit on either. An unknown value is a clean error.
//   @param max_results - Optional cap on hits (default 1000); results_truncated says whether the scan stopped early.
//   @param world - Optional world selector: absent/'editor' (default) | 'pie' (ambiguous: PlayWorld, else the first PIE context) | 'game' (alias of 'pie') | 'server' (the PIE world with authority, GetNetMode() != NM_Client) | 'client' | 'pie:N' (the PIE context whose PIEInstance is N). Case-insensitive; an unrecognised value is a clean error listing the worlds present, never a silent fallback to the editor world.
//   @returns { matching_actors: [ { name, actor_name, label, class, location, rotation, scale, is_loaded } ], matching_actors_count: int, search_pattern: string, match: string, results_truncated: bool, max_results: int, world: string (the normalized selector that resolved), world_name: string, world_package: string (in World Partition the UEDPIE_N prefix lives HERE, not in world_name), world_type: string, net_mode: string, pie_instance: int }

TSharedPtr<FJsonObject> HandleFindActorsByName(const TSharedPtr<FJsonObject>& Params)
{
    FString Needle;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pattern"), Needle, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // Which identifier is searched: the internal object name ('name'), the outliner
    // label agents assign ('label'), or either one ('both', the default).
    FString Scope = TEXT("both");
    Params->TryGetStringField(TEXT("match"), Scope);
    Scope = Scope.ToLower();
    const bool bTestName = Scope == TEXT("both") || Scope == TEXT("name");
    const bool bTestLabel = Scope == TEXT("both") || Scope == TEXT("label");
    if (!(bTestName || bTestLabel))
    {
        return mcp_common::MakeErrorReply(TEXT("Parameter 'match' must be one of: 'name', 'label', 'both'"));
    }

    int32 Cap = 1000;
    double CapFromJson = 0.0;
    if (Params->TryGetNumberField(TEXT("max_results"), CapFromJson))
    {
        Cap = static_cast<int32>(CapFromJson);
    }

    if (Needle.IsEmpty())
    {
        return UEAgentEditorCommandsImpl::EmptyParamReply(TEXT("pattern"));
    }
    if (Cap <= 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Parameter 'max_results' must be greater than 0"));
    }

    // The editor world unless `world` names another; an unknown selector FAILS rather than
    // falling back to the editor.
    FString SelectorUsed;
    FString ResolveFailure;
    UWorld* World = ResolveEditorOrPieWorld(Params, SelectorUsed, ResolveFailure);
    if (!World)
    {
        return mcp_common::MakeErrorReply(ResolveFailure);
    }

    // Plain case-insensitive substring test; no wildcards, no regex.
    TArray<TSharedPtr<FJsonValue>> Hits;
    bool bStoppedEarly = false;
    for (AActor* Each : UEAgentEditorCommandsImpl::SnapshotActors(World))
    {
        if (Each == nullptr)
        {
            continue;
        }
        const FString ObjectName = Each->GetName();
        const FString OutlinerLabel = Each->GetActorLabel();
        const bool bMatches = (bTestName && ObjectName.Contains(Needle))
                           || (bTestLabel && OutlinerLabel.Contains(Needle));
        if (!bMatches)
        {
            continue;
        }
        if (Hits.Num() >= Cap)
        {
            // A further hit exists past the cap: report the truncation instead of hiding it.
            bStoppedEarly = true;
            break;
        }

        // The usual actor JSON (name, class, location, rotation, scale) plus the explicit
        // internal name, label and load state.
        TSharedPtr<FJsonObject> Row = mcp_common::DescribeActorObject(Each);
        if (!Row.IsValid())
        {
            Row = MakeShared<FJsonObject>();
        }
        Row->SetStringField(TEXT("actor_name"), ObjectName);
        Row->SetStringField(TEXT("label"), OutlinerLabel);
        Row->SetBoolField(TEXT("is_loaded"), IsValid(Each));
        Hits.Add(MakeShared<FJsonValueObject>(Row));
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetArrayField(TEXT("matching_actors"), Hits);
    Reply->SetNumberField(TEXT("matching_actors_count"), Hits.Num());
    Reply->SetStringField(TEXT("search_pattern"), Needle);
    Reply->SetStringField(TEXT("match"), Scope);
    Reply->SetBoolField(TEXT("results_truncated"), bStoppedEarly);
    Reply->SetNumberField(TEXT("max_results"), Cap);
    Reply->SetStringField(TEXT("world"), SelectorUsed);
    // Which world was walked: world_name, world_package, world_type, net_mode, pie_instance.
    mcp_common::AddWorldEvidence(Reply, World);
    return Reply;
}

// MCP:spawn_actor - Place an actor of ANY placeable class -- a built-in short name (StaticMeshActor, PointLight, SpotLight, DirectionalLight, RectLight, SkyLight, CameraActor, Character), another engine or plugin class by name or path (SkyAtmosphere, ExponentialHeightFog, VolumetricCloud, SceneCapture2D, TextRenderActor, NiagaraActor, /Script/Engine.DecalActor...) or a Blueprint asset path -- into the editor level or a live PIE world, at an optional location/rotation/scale, and set its properties in the same call: properties={"Prop": v} on the actor, {"Component.Prop": v} on a component named so or of a class only one component has (e.g. {"ExponentialHeightFogComponent.FogDensity": 0.05}), written as set_properties writes them. A volume needs a brush: spawn_box_volume places one; an abstract or non-placeable class is refused. Refuses a name an actor already has (omit name to let the engine pick a unique one); the spawn is undoable. A property that cannot be written makes the call fail with properties_failed while the actor stays, so fix it with set_properties or remove it with delete_actor. It creates the actor in MEMORY only -- save_level (with the .umap writable) puts it on disk. WARNING: with 2+ PIE clients world='pie' can land in the CLIENT world, producing an actor the authority never sees -- ask for world='server' instead, and check has_authority and world_package in the reply.
//   @pack actors
//   @param type - The actor class: a built-in short name (StaticMeshActor, PointLight, SpotLight, DirectionalLight, RectLight, SkyLight, CameraActor, Character; a light also as Point, Spot, Directional, Rect or Sky), any other actor class by name or /Script path, or a Blueprint asset path (/Game/BP_Door). Volumes go through spawn_box_volume.
//   @param name - Optional internal name (and label) for the new actor; must be unused in the target world or the call is refused. When omitted the engine names the actor uniquely and the label is the class default.
//   @param location - Optional world location, ARRAY form only: [x, y, z]; the origin when omitted. Any other value (an object, another length) is an error and nothing is spawned.
//   @param rotation - Optional world rotation as the array [pitch, yaw, roll] -- note index 2 is ROLL, not yaw -- or, unambiguously, the object {"pitch":..,"yaw":..,"roll":..}, which is the recommended form. Defaults to zero. Directional, spot and rect lights shine along its forward axis.
//   @param scale - Optional world scale, ARRAY form only: [x, y, z]. Defaults to 1,1,1; any other value (an object, another length) is an error and nothing is spawned.
//   @param static_mesh - Optional UStaticMesh asset path, only honoured for type=StaticMeshActor (e.g. /Engine/BasicShapes/Cube).
//   @param intensity - Optional, light types only: the light component's Intensity in the light's units (Directional: lux; Sky: a multiplier); engine default when omitted. Sent for a non-light type it is refused and nothing is spawned.
//   @param color - Optional, light types only: [r, g, b] linear color, usually 0..1; engine default (white) when omitted. Sent for a non-light type it is refused and nothing is spawned.
//   @param properties [object] - Optional {"Prop": value, "Component.Prop": value} written after the spawn with set_properties' writer: engine property names, a struct as a JSON object of the fields to change.
//   @param world - Optional world selector for WHERE the actor is spawned: absent/'editor' (default) | 'pie' (ambiguous: PlayWorld, else the first PIE context) | 'game' (alias of 'pie') | 'server' (the PIE world with authority -- what you want for anything replicated) | 'client' | 'pie:N'. Case-insensitive; an unrecognised value is a clean error listing the worlds present, never a silent spawn into the editor world.
//   @returns { actor JSON via DescribeActorObject: name, class, location, rotation, scale, plus world: string (the normalized selector that resolved), world_name: string, world_package: string (the UEDPIE_N prefix lives HERE), world_type: string, net_mode: string, pie_instance: int, has_authority: bool, local_role: string, light_intensity?: number (light types: the intensity now on the component), light_color?: [r, g, b] (light types: the color now on the component), properties_set?: [string], properties_failed?: [{property, error}] (with properties) }

TSharedPtr<FJsonObject> HandleSpawnActor(const TSharedPtr<FJsonObject>& Params)
{
    FString TypeName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("type"), TypeName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    // The name is optional: without one the engine picks a unique name and none of the name preflight applies.
    FString NewName;
    const bool bNamed = Params->TryGetStringField(TEXT("name"), NewName);
    if (TypeName.IsEmpty())
    {
        return UEAgentEditorCommandsImpl::EmptyParamReply(TEXT("type"));
    }
    if (bNamed && NewName.IsEmpty())
    {
        return UEAgentEditorCommandsImpl::EmptyParamReply(TEXT("name"));
    }

    UClass* SpawnClass = UEAgentEditorCommandsImpl::BuiltInSpawnClass(TypeName);
    const bool bBuiltIn = SpawnClass != nullptr;
    if (SpawnClass == nullptr)
    {
        // Any other actor class: a native name or path (SkyAtmosphere, /Script/Engine.SkyAtmosphere) or a Blueprint.
        SpawnClass = FUEAgentGraphUtils::ResolveClass(TypeName);
        if (SpawnClass == nullptr)
        {
            return mcp_common::MakeErrorReply(FUEAgentGraphUtils::DescribeClassNotFound(TypeName)
                + TEXT(". type takes an actor class: a native name or path, or a Blueprint asset path; nothing was spawned"));
        }
        if (!SpawnClass->IsChildOf(AActor::StaticClass()))
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s is not an actor class; nothing was spawned"), *SpawnClass->GetPathName()));
        }
        if (SpawnClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_NotPlaceable))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("%s cannot be placed in a level (it is abstract, deprecated, replaced or not placeable); nothing was spawned"), *SpawnClass->GetPathName()));
        }
        if (SpawnClass->IsChildOf(AVolume::StaticClass()))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("%s is a volume, which needs a brush to enclose anything: spawn_box_volume volume_class=%s builds one. Nothing was spawned"),
                *SpawnClass->GetName(), *SpawnClass->GetName()));
        }
    }

    // Optional light seeding, refused up front on a type that has no light component to take it.
    double LightIntensity = 0.0;
    const bool bHasIntensity = Params->TryGetNumberField(TEXT("intensity"), LightIntensity);
    const bool bHasColor = Params->HasField(TEXT("color"));
    const bool bIsLightType = SpawnClass->IsChildOf(ALight::StaticClass()) || SpawnClass->IsChildOf(ASkyLight::StaticClass());
    if ((bHasIntensity || bHasColor) && !bIsLightType)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("intensity and color apply only to a light type (PointLight, SpotLight, DirectionalLight, RectLight, SkyLight), not %s; nothing was spawned"),
            *TypeName));
    }
    const FVector ColorRgb = bHasColor ? mcp_common::ReadJsonVector(Params, TEXT("color")) : FVector::OneVector;

    // Optional placement. SpawnActor takes the location and rotation; the scale is
    // applied once the actor exists.
    const FVector SpawnAt = Params->HasField(TEXT("location"))
        ? mcp_common::ReadJsonVector(Params, TEXT("location"))
        : FVector::ZeroVector;
    const FRotator SpawnFacing = Params->HasField(TEXT("rotation"))
        ? mcp_common::ReadJsonRotator(Params, TEXT("rotation"))
        : FRotator::ZeroRotator;
    const FVector SpawnScale = Params->HasField(TEXT("scale"))
        ? mcp_common::ReadJsonVector(Params, TEXT("scale"))
        : FVector::OneVector;

    // Where the actor is born: the editor world by default, and 'server' / 'client' /
    // 'pie:N' in that live world -- never the editor world in their place, which would be an
    // actor the authority never sees.
    FString SelectorUsed;
    FString ResolveFailure;
    UWorld* World = ResolveEditorOrPieWorld(Params, SelectorUsed, ResolveFailure);
    if (!World)
    {
        return mcp_common::MakeErrorReply(ResolveFailure);
    }

    if (bNamed && mcp_common::FindActorByObjectName(World, NewName) != nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor with name '%s' already exists"), *NewName));
    }

    // The live-actor check above is not what the engine checks. SpawnActor asks
    // StaticFindObjectFast(nullptr, LevelToSpawnIn, Name) -- any class, garbage included
    // (UObjectGlobals.cpp) -- and a hit there is UE_LOG(LogSpawn, Fatal, "Cannot
    // generate unique name for '%ls'...") in LevelActor.cpp: the process dies.
    //
    // Nothing exotic triggers it. delete_actor destroys the actor, but its name stays taken:
    // the corpse is still outered to the ULevel, and the delete is transacted, so the undo
    // buffer keeps it alive. spawn X -> delete X -> spawn X therefore hits it every time.
    //
    // So ask exactly the engine's question (class nullptr, not AActor::StaticClass()) of
    // exactly the level it will spawn into: SpawnParams sets neither OverrideLevel nor Owner,
    // which makes that World->GetCurrentLevel() (LevelActor.cpp).
    if (ULevel* SpawnLevel = bNamed ? World->GetCurrentLevel() : nullptr)
    {
        if (UObject* NameHolder = StaticFindObjectFast(nullptr, SpawnLevel, FName(*NewName)))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("REFUSED: the name '%s' is still held in level '%s' by %s (%s%s). ")
                TEXT("Spawning over it would hit UE_LOG(Fatal) in LevelActor.cpp and KILL THE EDITOR. ")
                TEXT("Use a different name; the holder is usually an actor you deleted -- delete_actor is ")
                TEXT("transacted, so the undo buffer keeps the corpse (and its name) alive. To reclaim the ")
                TEXT("name: reset_transaction_buffer (IRREVERSIBLE: wipes all undo/redo) and then force a GC."),
                *NewName, *SpawnLevel->GetName(), *NameHolder->GetName(),
                *NameHolder->GetClass()->GetName(),
                IsValid(NameHolder) ? TEXT("") : TEXT(", already destroyed/pending GC")));
        }
    }

    // Undoable: the spawn records into this transaction.
    FScopedTransaction SpawnTransaction(UEAgentEditorCommandsImpl::UndoTitle(TEXT("Spawn Actor"), bNamed ? NewName : TypeName));

    FActorSpawnParameters SpawnParams;
    if (!bBuiltIn)
    {
        // As the class-specific spawners did: a placed actor is wanted where it was asked, overlapping or not.
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    }
    if (bNamed)
    {
        SpawnParams.Name = FName(*NewName);
        // Should the preflight above ever miss a clash (another current level, a race with
        // the GC), the engine then returns null and logs an error instead of aborting the
        // process; the default NameMode is Required_Fatal.
        SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Required_ErrorAndReturnNull;
    }

    AActor* Spawned = World->SpawnActor(SpawnClass, &SpawnAt, &SpawnFacing, SpawnParams);

    // A static mesh actor can get its mesh right away.
    if (AStaticMeshActor* MeshHost = Cast<AStaticMeshActor>(Spawned))
    {
        FString MeshAsset;
        if (Params->TryGetStringField(TEXT("static_mesh"), MeshAsset))
        {
            if (UStaticMesh* Mesh = UEAgentEditorCommandsImpl::LoadMeshForSpawn(MeshAsset))
            {
                MeshHost->GetStaticMeshComponent()->SetStaticMesh(Mesh);
            }
            else
            {
                UE_LOG(LogTemp, Warning, TEXT("spawn_actor: no static mesh loads from '%s'; spawned without one"), *MeshAsset);
            }
        }
    }

    if (Spawned == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create actor"));
    }

    FTransform Placed = Spawned->GetTransform();
    Placed.SetScale3D(SpawnScale);
    Spawned->SetActorTransform(Placed);

    // The documented contract is "label = name". SpawnParams.Name only sets the internal
    // object name, so handlers that resolve by label would not find the actor. Unnamed, the
    // editor's default label stays.
    if (bNamed)
    {
        Spawned->SetActorLabel(NewName);
    }

    // The light seeding, on whichever component the light carries, read back from it.
    const FLinearColor LightColor(ColorRgb.X, ColorRgb.Y, ColorRgb.Z);
    TOptional<float> IntensityNow;
    TOptional<FLinearColor> ColorNow;
    if (ASkyLight* Sky = Cast<ASkyLight>(Spawned))
    {
        if (USkyLightComponent* SkyComponent = Sky->GetLightComponent())
        {
            if (bHasIntensity) { SkyComponent->SetIntensity(static_cast<float>(LightIntensity)); }
            if (bHasColor)     { SkyComponent->SetLightColor(LightColor); }
            SkyComponent->MarkRenderStateDirty();
            IntensityNow = SkyComponent->Intensity;
            ColorNow = SkyComponent->GetLightColor();
        }
    }
    else if (ALight* Light = Cast<ALight>(Spawned))
    {
        if (ULightComponent* LightComponent = Light->GetLightComponent())
        {
            if (bHasIntensity) { LightComponent->SetIntensity(static_cast<float>(LightIntensity)); }
            if (bHasColor)     { LightComponent->SetLightColor(LightColor); }
            IntensityNow = LightComponent->Intensity;
            ColorNow = LightComponent->GetLightColor();
        }
    }

    // Stamped with the EXACT world the actor landed in; otherwise a spawn into the client
    // (or the editor) looks like one that worked. world_package is the tell: in World
    // Partition only the package carries the UEDPIE_N prefix.
    TSharedPtr<FJsonObject> Reply = mcp_common::DescribeActorObject(Spawned, true);
    if (!Reply.IsValid())
    {
        Reply = MakeShared<FJsonObject>();
    }
    Reply->SetStringField(TEXT("world"), SelectorUsed);
    mcp_common::AddWorldEvidence(Reply, World);
    // Its real authority: a client-world spawn exists for nobody else, yet would otherwise look like a server one.
    Reply->SetBoolField(TEXT("has_authority"), Spawned->HasAuthority());
    if (const UEnum* NetRoleEnum = StaticEnum<ENetRole>())
    {
        Reply->SetStringField(TEXT("local_role"), NetRoleEnum->GetNameStringByValue(static_cast<int64>(Spawned->GetLocalRole())));
    }
    if (IntensityNow.IsSet())
    {
        Reply->SetNumberField(TEXT("light_intensity"), IntensityNow.GetValue());
    }
    if (ColorNow.IsSet())
    {
        const FLinearColor& C = ColorNow.GetValue();
        Reply->SetArrayField(TEXT("light_color"), {
            MakeShared<FJsonValueNumber>(C.R), MakeShared<FJsonValueNumber>(C.G), MakeShared<FJsonValueNumber>(C.B) });
    }

    // properties: written once the actor exists, through set_properties' writer (Details-panel semantics), on the
    // actor ("Prop") or on one of its components ("Component.Prop", by name or by a class only one component has).
    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && PropsObj->IsValid() && (*PropsObj)->Values.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> SetNames;
        TArray<TSharedPtr<FJsonValue>> Failed;
        auto AddFailure = [&Failed](const FString& Key, const FString& Error)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("property"), Key);
            Entry->SetStringField(TEXT("error"), Error);
            Failed.Add(MakeShared<FJsonValueObject>(Entry));
        };
        // Grouped by the object they go to, in the order given.
        TArray<UObject*> Targets;
        TArray<TSharedPtr<FJsonObject>> TargetProperties;
        TArray<FString> TargetPrefixes;
        for (const auto& Entry : (*PropsObj)->Values)
        {
            const TPair<FString, TSharedPtr<FJsonValue>> Pair(FString(*Entry.Key), Entry.Value);
            UObject* Target = Spawned;
            FString Prefix;
            FString PropertyName = Pair.Key;
            int32 Dot = INDEX_NONE;
            if (Pair.Key.FindChar(TEXT('.'), Dot))
            {
                TArray<FString> Ambiguous;
                Prefix = Pair.Key.Left(Dot);
                PropertyName = Pair.Key.Mid(Dot + 1);
                Target = mcp_common::FindComponentByNameOrClass(Spawned, Prefix, &Ambiguous);
                if (!Target)
                {
                    AddFailure(Pair.Key, Ambiguous.Num() > 0
                        ? FString::Printf(TEXT("%d components are of class %s (%s); name one"), Ambiguous.Num(), *Prefix, *FString::Join(Ambiguous, TEXT(", ")))
                        : FString::Printf(TEXT("the actor has no component named %s, nor exactly one of that class"), *Prefix));
                    continue;
                }
            }
            int32 TargetIndex = Targets.IndexOfByKey(Target);
            if (TargetIndex == INDEX_NONE)
            {
                TargetIndex = Targets.Add(Target);
                TargetProperties.Add(MakeShared<FJsonObject>());
                TargetPrefixes.Add(Prefix.IsEmpty() ? FString() : Prefix + TEXT("."));
            }
            TargetProperties[TargetIndex]->SetField(PropertyName, Pair.Value);
        }
        for (int32 TargetIndex = 0; TargetIndex < Targets.Num(); ++TargetIndex)
        {
            const FString& Prefix = TargetPrefixes[TargetIndex];
            TSharedPtr<FJsonObject> WriteParams = MakeShared<FJsonObject>();
            WriteParams->SetStringField(TEXT("object_path"), Targets[TargetIndex]->GetPathName());
            WriteParams->SetObjectField(TEXT("properties"), TargetProperties[TargetIndex]);
            const TSharedPtr<FJsonObject> Written = McpCommandFamilies::Run(TEXT("set_properties"), WriteParams);
            const TSharedPtr<FJsonObject>* Data = nullptr;
            const TSharedPtr<FJsonObject> Outcome = Written.IsValid() && Written->TryGetObjectField(TEXT("data"), Data) && Data ? *Data : Written;
            const TArray<TSharedPtr<FJsonValue>>* SetList = nullptr;
            const TArray<TSharedPtr<FJsonValue>>* FailedList = nullptr;
            const bool bHasSet = Outcome.IsValid() && Outcome->TryGetArrayField(TEXT("set"), SetList) && SetList;
            const bool bHasFailed = Outcome.IsValid() && Outcome->TryGetArrayField(TEXT("failed"), FailedList) && FailedList;
            if (bHasSet)
            {
                for (const TSharedPtr<FJsonValue>& Name : *SetList)
                {
                    SetNames.Add(MakeShared<FJsonValueString>(Prefix + Name->AsString()));
                }
            }
            if (bHasFailed)
            {
                for (const TSharedPtr<FJsonValue>& Entry : *FailedList)
                {
                    const TSharedPtr<FJsonObject>* EntryObj = nullptr;
                    if (Entry->TryGetObject(EntryObj) && EntryObj)
                    {
                        AddFailure(Prefix + (*EntryObj)->GetStringField(TEXT("property")), (*EntryObj)->GetStringField(TEXT("error")));
                    }
                }
            }
            if (!bHasSet && !bHasFailed)
            {
                FString Error;
                if (Written.IsValid())
                {
                    Written->TryGetStringField(TEXT("error"), Error);
                }
                for (const auto& Pair : TargetProperties[TargetIndex]->Values)
                {
                    AddFailure(Prefix + FString(*Pair.Key), Error.IsEmpty() ? FString(TEXT("not written")) : Error);
                }
            }
        }
        Reply->SetArrayField(TEXT("properties_set"), SetNames);
        Reply->SetArrayField(TEXT("properties_failed"), Failed);
        if (Failed.Num() > 0)
        {
            return mcp_common::MakeOutcomeReply(Reply, false, FString::Printf(
                TEXT("%s was spawned, but %d of its properties were not written (properties_failed says why). The actor exists: fix them with set_properties, or remove it with delete_actor."),
                *Spawned->GetActorLabel(), Failed.Num()));
        }
    }
    return Reply;
}

// MCP:delete_actor - Remove one actor from the EDITOR level through the editor's own delete path (select + edactDeleteSelected), so it is transacted and undoable, and echo back the deleted actor's JSON. WARNING: two edges that bite: it matches the INTERNAL name only -- unlike set_actor_transform, an editor LABEL will not resolve -- and it clears the current selection as a side effect. It cannot touch PIE actors (editor world only), and the deletion lives in memory until save_level writes the .umap. Use delete_actors for several at once.
//   @pack actors
//   @param name - Actor's internal name (GetName), case-insensitive but WHOLE-string. Editor labels are NOT accepted here; resolve one first with find_actors_by_name.
//   @returns { deleted_actor: actor JSON captured before the delete }

TSharedPtr<FJsonObject> HandleDeleteActor(const TSharedPtr<FJsonObject>& Params)
{
    FString DoomedName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), DoomedName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (DoomedName.IsEmpty())
    {
        return UEAgentEditorCommandsImpl::EmptyParamReply(TEXT("name"));
    }

    UWorld* EditorWorld = UEAgentEditorCommandsImpl::ValidEditorWorld();
    if (EditorWorld == nullptr)
    {
        return UEAgentEditorCommandsImpl::NoWorldReply();
    }

    // The whole removal is one undo step.
    FScopedTransaction DeleteTransaction(UEAgentEditorCommandsImpl::UndoTitle(TEXT("Delete Actor"), DoomedName));

    AActor* Doomed = mcp_common::FindActorByObjectName(EditorWorld, DoomedName);
    if (Doomed == nullptr)
    {
        return UEAgentEditorCommandsImpl::ActorNotFoundReply(DoomedName);
    }

    // The reply describes the actor as it was, so capture it before it goes away.
    TSharedPtr<FJsonObject> Removed = mcp_common::DescribeActorObject(Doomed);

    if (GEditor != nullptr)
    {
        // The editor's own delete path, which works on the selection: references, selection
        // state and the transaction are then handled exactly as for a user delete.
        UEAgentEditorCommandsImpl::ClearEditorSelection();
        UEAgentEditorCommandsImpl::AddToEditorSelection(Doomed, /*bNotify=*/true);
        GEditor->edactDeleteSelected(EditorWorld);
    }
    else
    {
        // No editor: destroy directly, still inside the transaction.
        Doomed->Destroy();
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetObjectField(TEXT("deleted_actor"), Removed);
    return Reply;
}

// MCP:set_actor_transform - Move, rotate and/or scale ONE actor (omitted components are left as they are) and return the actor's re-read transform plus the identity of the world that was written. Undoable, and it calls Modify() first -- which matters, because moving an actor from Python does NOT dirty its OFPA package and then save_level reports success while writing nothing. Takes a `world` selector, so it also reaches a LIVE PIE pawn (an editor-only lookup would answer "Actor not found" for an actor that exists in another world). Use execute_batch for many actors, snap_actor_to_landscape to drop one onto the landscape.
//   @param name - Actor's internal name (GetName) or editor label (GetActorLabel), case-insensitive whole-string match.
//   @param world - Optional: "editor" (default) | "pie" | "server" | "client" | "pie:N". In co-op the authoritative pawn lives on the server, and a move written to the client world will be corrected by replication.
//   @param location - Optional new WORLD location, ARRAY form only: [x, y, z]. Absolute, not a delta; any other value (an object, another length) is an error and the actor is not touched.
//   @param rotation - Optional new WORLD rotation as the array [pitch, yaw, roll] -- note index 2 is ROLL, not yaw -- or, unambiguously, the object {"pitch":..,"yaw":..,"roll":..}, which is the recommended form.
//   @param scale - Optional new WORLD scale, ARRAY form only: [x, y, z]. Any other value (an object, another length) is an error and the actor is not touched.
//   @returns { actor JSON via DescribeActorObject, transform RE-READ off the actor after the write }

TSharedPtr<FJsonObject> HandleSetActorTransform(const TSharedPtr<FJsonObject>& Params)
{
    FString TargetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), TargetName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TargetName.IsEmpty())
    {
        return UEAgentEditorCommandsImpl::EmptyParamReply(TEXT("name"));
    }

    // Any world, not only the editor one, like its reading sibling (get_actor_properties_by_class).
    // Pinned to the editor, it could not move a PIE actor, and "Actor not found: X" would read
    // as "that actor does not exist": that asymmetry is exactly what manufactures false negatives.
    FString WorldError;
    UWorld* World = mcp_common::ResolveWorldFromParams(Params, WorldError);
    if (World == nullptr || !World->IsValidLowLevel())
    {
        return mcp_common::MakeErrorReply(WorldError.IsEmpty() ? TEXT("No valid world found") : WorldError);
    }

    // Internal name (e.g. "Landscape_UAID_...") OR editor label, through the resolver the
    // rest of the plugin uses -- the landscape commands accept both, and so does this one.
    AActor* Target = mcp_common::FindActorByName(World, TargetName);
    if (Target == nullptr)
    {
        // The miss SAYS WHERE IT LOOKED; otherwise a lookup in the wrong world reads as
        // "the actor does not exist".
        FString RequestedWorld = TEXT("editor");
        Params->TryGetStringField(TEXT("world"), RequestedWorld);
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Actor not found: %s (searched world='%s', package '%s'). If the actor is alive in "
                 "Play-in-Editor, pass world='server' (or 'client'/'pie:N')."),
            *TargetName, *RequestedWorld, *World->GetOutermost()->GetName()));
    }

    FScopedTransaction MoveTransaction(UEAgentEditorCommandsImpl::UndoTitle(TEXT("Set Actor Transform"), TargetName));
    // Modify() snapshots the old state into the transaction AND dirties the actor's package
    // (it cascades to the root component). SetActorTransform does neither, and without the
    // dirty flag save_level skips the actor's OFPA package: a save that looks successful
    // but loses the move.
    Target->Modify();

    FTransform Updated = Target->GetTransform();
    UEAgentEditorCommandsImpl::OverlayTransformFields(Params, Updated);
    Target->SetActorTransform(Updated);

    // Re-read off the actor, stamped with WHICH world was written: two tools resolving
    // "pie" to different worlds otherwise produce contradictions that look like a game bug.
    TSharedPtr<FJsonObject> Reply = mcp_common::DescribeActorObject(Target, true);
    mcp_common::AddWorldEvidence(Reply, World);
    return Reply;
}

// The editor's actor selection

// MCP:select_actors - Select actors by name in the editor viewport/outliner, appending to or replacing the current selection, and report which ones were not found. Use it to stage the input for the selection-driven tools (copy_actors with copy_selected, move_actors_to_level) or to show the user what you mean before they look. WARNING: matches INTERNAL names only (no editor labels) and misses are reported in not_found_names instead of failing, so check not_found_count. Editor world only.
//   @param actor_names - Array of internal actor names (GetName), case-insensitive whole-string. Editor labels do NOT resolve here; convert them with find_actors_by_name.
//   @param add_to_selection - Optional bool (default TRUE = append to whatever is already selected). Pass false to clear the selection first and end up with exactly these actors.
//   @returns { selected_actors: [actor JSON], selected_count: int, not_found_count: int, not_found_names?: [string], add_to_selection: bool }

TSharedPtr<FJsonObject> HandleSelectActors(const TSharedPtr<FJsonObject>& Params)
{
    const TArray<TSharedPtr<FJsonValue>>* RequestedNames = nullptr;
    if (!Params->TryGetArrayField(TEXT("actor_names"), RequestedNames) || !RequestedNames || RequestedNames->Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: actor_names (array of actor names)"));
    }

    // By default the named actors are added to whatever is already selected.
    bool bAppend = true;
    Params->TryGetBoolField(TEXT("add_to_selection"), bAppend);

    UWorld* EditorWorld = UEAgentEditorCommandsImpl::ValidEditorWorld();
    if (EditorWorld == nullptr)
    {
        return UEAgentEditorCommandsImpl::NoWorldReply();
    }

    FScopedTransaction SelectTransaction(FText::FromString(TEXT("Select Actors")));

    if (!bAppend && GEditor != nullptr)
    {
        UEAgentEditorCommandsImpl::ClearEditorSelection();
    }

    // Internal names only; a miss is reported back rather than treated as an error.
    TArray<FString> Unmatched;
    const TArray<AActor*> Picked = UEAgentEditorCommandsImpl::ResolveNameList(
        *RequestedNames, UEAgentEditorCommandsImpl::SnapshotActors(EditorWorld), &Unmatched);

    TArray<TSharedPtr<FJsonValue>> PickedJson;
    for (AActor* Each : Picked)
    {
        if (GEditor != nullptr)
        {
            UEAgentEditorCommandsImpl::AddToEditorSelection(Each, /*bNotify=*/false);
        }
        PickedJson.Add(mcp_common::DescribeActorValue(Each));
    }
    // One notification for the whole batch, so viewports and outliner refresh once.
    if (GEditor != nullptr && Picked.Num() > 0)
    {
        GEditor->NoteSelectionChange();
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetArrayField(TEXT("selected_actors"), PickedJson);
    Reply->SetNumberField(TEXT("selected_count"), Picked.Num());
    Reply->SetNumberField(TEXT("not_found_count"), Unmatched.Num());
    if (Unmatched.Num() > 0)
    {
        Reply->SetArrayField(TEXT("not_found_names"), mcp_common::StringsToJson(Unmatched));
    }
    Reply->SetBoolField(TEXT("add_to_selection"), bAppend);
    return Reply;
}

// MCP:deselect_actors - Drop specific actors from the editor selection, or clear it entirely with deselect_all. The counterpart of select_actors; use the clear-all form to leave the editor in a known state before a tool that acts on the selection (copy_actors, move_actors_to_level) so it does not pick up leftovers. Editor world only; matches INTERNAL names.
//   @param actor_names - Optional array of internal actor names (GetName) to remove from the selection. One of actor_names or deselect_all=true is required.
//   @param deselect_all - Optional bool. True clears the whole selection; it wins over actor_names when both are sent.
//   @returns { deselected_all: bool, deselected_count: int }

TSharedPtr<FJsonObject> HandleDeselectActors(const TSharedPtr<FJsonObject>& Params)
{
    // Either/or: neither read is a guard of its own, so the catalog publishes both as optional
    // and the check below is the one that fails the call.
    const TArray<TSharedPtr<FJsonValue>>* NamesToDrop = nullptr;
    Params->TryGetArrayField(TEXT("actor_names"), NamesToDrop);
    bool bClearAll = false;
    Params->TryGetBoolField(TEXT("deselect_all"), bClearAll);
    if (NamesToDrop == nullptr && !bClearAll)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: actor_names (array) or deselect_all (true)"));
    }

    UWorld* EditorWorld = UEAgentEditorCommandsImpl::ValidEditorWorld();
    if (EditorWorld == nullptr)
    {
        return UEAgentEditorCommandsImpl::NoWorldReply();
    }

    FScopedTransaction DeselectTransaction(FText::FromString(TEXT("Deselect Actors")));

    int32 Dropped = 0;
    if (bClearAll)
    {
        if (GEditor != nullptr)
        {
            // Count what was selected before wiping it.
            const USelection* Current = GEditor->GetSelectedActors();
            Dropped = (Current != nullptr) ? Current->Num() : 0;
            UEAgentEditorCommandsImpl::ClearEditorSelection();
            GEditor->NoteSelectionChange();
        }
    }
    else if (NamesToDrop != nullptr)
    {
        const TArray<AActor*> Named = UEAgentEditorCommandsImpl::ResolveNameList(
            *NamesToDrop, UEAgentEditorCommandsImpl::SnapshotActors(EditorWorld), nullptr);
        for (AActor* Each : Named)
        {
            // Only an actor that really is selected counts as deselected.
            if (GEditor != nullptr && GEditor->GetSelectedActors()->IsSelected(Each))
            {
                GEditor->SelectActor(Each, /*bInSelected=*/false, /*bNotify=*/false, /*bSelectEvenIfHidden=*/true);
                ++Dropped;
            }
        }
        if (GEditor != nullptr && Dropped > 0)
        {
            GEditor->NoteSelectionChange();
        }
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetBoolField(TEXT("deselected_all"), bClearAll);
    Reply->SetNumberField(TEXT("deselected_count"), Dropped);
    return Reply;
}

// MCP:get_selected_actors - Read what is selected in the editor viewport/outliner right now: each actor's JSON plus the count and the world it belongs to. Use it to act on "what the user just clicked" without asking for names, and as the read-back for select_actors / deselect_actors. Editor world only, and remember that delete_actor, duplicate_actor, copy_actors and move_actors_to_level all MUTATE the selection as a side effect -- read it before those, not after.
//   @returns { selected_actors: [actor JSON], selected_count: int, world_name: string }

TSharedPtr<FJsonObject> HandleGetSelectedActors(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* EditorWorld = UEAgentEditorCommandsImpl::ValidEditorWorld();
    if (EditorWorld == nullptr)
    {
        return UEAgentEditorCommandsImpl::NoWorldReply();
    }

    // Selected objects that are actors of the editor world; anything else is left out.
    USelection* EditorSelection = GEditor ? GEditor->GetSelectedActors() : nullptr;
    const TArray<TSharedPtr<FJsonValue>> SelectedJson = UEAgentEditorCommandsImpl::ActorsToJson(
        UEAgentEditorCommandsImpl::SelectedActorsIn(EditorSelection, EditorWorld));

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetArrayField(TEXT("selected_actors"), SelectedJson);
    Reply->SetNumberField(TEXT("selected_count"), SelectedJson.Num());
    Reply->SetStringField(TEXT("world_name"), EditorWorld->GetName());
    return Reply;
}

// Cloning: in-place duplicate and the clipboard pair

// MCP:duplicate_actor - Copy one editor-world actor in place through the editor's duplicate path, keeping its component overrides, and return both the new actor's JSON and the original's. Undoable; it changes the editor SELECTION as a side effect. Matches the INTERNAL name only (no editor labels). The clone exists in memory until save_level writes the .umap -- and a save that answers success while `git status` stays clean means the .umap is read-only (e.g. a Git LFS `lockable` file you have not locked), not that the duplicate failed.
//   @param actor_name - Actor's internal name (GetName), case-insensitive whole-string. Editor labels are NOT accepted; resolve one with find_actors_by_name first.
//   @returns { duplicated_actor: actor JSON (its auto-assigned name lives here), original_actor: actor JSON }

TSharedPtr<FJsonObject> HandleDuplicateActor(const TSharedPtr<FJsonObject>& Params)
{
    FString SourceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), SourceName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (SourceName.IsEmpty())
    {
        return UEAgentEditorCommandsImpl::EmptyParamReply(TEXT("actor_name"));
    }

    UWorld* EditorWorld = nullptr;
    TSharedPtr<FJsonObject> Failure;
    AActor* Source = UEAgentEditorCommandsImpl::ResolveEditorWorldActor(SourceName, EditorWorld, Failure);
    if (Source == nullptr)
    {
        return Failure;
    }

    FScopedTransaction DuplicateTransaction(UEAgentEditorCommandsImpl::UndoTitle(TEXT("Duplicate Actor"), SourceName));
    if (GEditor == nullptr)
    {
        return UEAgentEditorCommandsImpl::NoEditorReply();
    }

    // The editor's duplicate command works on the selection: make the source the only
    // selected actor and duplicate it in place (no location offset). That path copies the
    // components and keeps references the way a user duplicate does.
    UEAgentEditorCommandsImpl::ClearEditorSelection();
    UEAgentEditorCommandsImpl::AddToEditorSelection(Source, /*bNotify=*/true);
    GEditor->edactDuplicateSelected(EditorWorld->GetCurrentLevel(), /*bOffsetLocations=*/false);

    // Afterwards the copy is what is selected.
    AActor* Copy = nullptr;
    for (AActor* Candidate : UEAgentEditorCommandsImpl::SelectedActorsIn(GEditor->GetSelectedActors(), EditorWorld))
    {
        if (Candidate != Source)
        {
            Copy = Candidate;
            break;
        }
    }
    if (Copy == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to duplicate actor"));
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetObjectField(TEXT("duplicated_actor"), mcp_common::DescribeActorObject(Copy));
    Reply->SetObjectField(TEXT("original_actor"), mcp_common::DescribeActorObject(Source));
    return Reply;
}

// MCP:copy_actors - Put editor-world actors on the editor CLIPBOARD (edactCopySelected), either by name or whatever is currently selected, so paste_actors can drop them back -- the way to clone a group while keeping their relative layout, which duplicate_actor (one actor, in place) does not do. WARNING: naming them REPLACES the current selection as a side effect, and matching is by INTERNAL name only. One of actor_names or copy_selected=true is required.
//   @param actor_names - Optional array of internal actor names (GetName) to copy; supplying it re-selects exactly those actors first.
//   @param copy_selected - Optional bool. True (with actor_names omitted) copies whatever is selected right now, leaving the selection alone.
//   @returns { copied_count: int, success: bool (hardcoded; verify with paste_actors' pasted_count) }

TSharedPtr<FJsonObject> HandleCopyActors(const TSharedPtr<FJsonObject>& Params)
{
    // Either/or, checked here rather than by a guarded read, so neither param is published as
    // required (the server would otherwise refuse a call that sends only copy_selected).
    const TArray<TSharedPtr<FJsonValue>>* NamesToCopy = nullptr;
    Params->TryGetArrayField(TEXT("actor_names"), NamesToCopy);
    bool bUseCurrentSelection = false;
    if (NamesToCopy == nullptr || NamesToCopy->Num() == 0)
    {
        // No usable list: copying the current selection is allowed, but only on request.
        Params->TryGetBoolField(TEXT("copy_selected"), bUseCurrentSelection);
        if (!bUseCurrentSelection)
        {
            return mcp_common::MakeErrorReply(TEXT("Missing required parameter: actor_names (array) or copy_selected (true)"));
        }
    }

    UWorld* EditorWorld = UEAgentEditorCommandsImpl::ValidEditorWorld();
    if (EditorWorld == nullptr)
    {
        return UEAgentEditorCommandsImpl::NoWorldReply();
    }
    if (GEditor == nullptr)
    {
        return UEAgentEditorCommandsImpl::NoEditorReply();
    }

    FScopedTransaction CopyTransaction(FText::FromString(TEXT("Copy Actors")));

    // The clipboard copy works on the selection: named actors REPLACE it, otherwise it is
    // copied as it stands.
    int32 CopyCount = 0;
    const bool bByName = NamesToCopy != nullptr && NamesToCopy->Num() > 0;
    if (bByName)
    {
        UEAgentEditorCommandsImpl::ClearEditorSelection();
        const TArray<AActor*> Chosen = UEAgentEditorCommandsImpl::ResolveNameList(
            *NamesToCopy, UEAgentEditorCommandsImpl::SnapshotActors(EditorWorld), nullptr);
        for (AActor* Each : Chosen)
        {
            UEAgentEditorCommandsImpl::AddToEditorSelection(Each, /*bNotify=*/false);
        }
        CopyCount = Chosen.Num();
    }
    else if (bUseCurrentSelection)
    {
        const USelection* Current = GEditor->GetSelectedActors();
        CopyCount = (Current != nullptr) ? Current->Num() : 0;
    }

    if (CopyCount == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("No actors to copy"));
    }
    GEditor->edactCopySelected(EditorWorld);

    // `success` is not measured here; paste_actors' pasted_count is the real check.
    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetNumberField(TEXT("copied_count"), CopyCount);
    Reply->SetBoolField(TEXT("success"), true);
    return Reply;
}

// MCP:paste_actors - Drop whatever copy_actors left on the editor clipboard into the current level (edactPasteSelected), optionally re-centring the whole group on a world location while KEEPING each actor's relative offset -- that is the point of the copy/paste pair versus duplicate_actor. The pasted actors end up selected and are returned with their new names and transforms. Editor world only; they live in memory until save_level writes the .umap.
//   @param location - Optional world point to centre the pasted group on, ARRAY form only [x, y, z] (the group's average is moved there, each actor's offset preserved). Omit to paste where the originals were; any other value (an object, another length) is an error and nothing is pasted.
//   @returns { pasted_actors: [actor JSON with the new auto-assigned names], pasted_count: int, success: bool }

TSharedPtr<FJsonObject> HandlePasteActors(const TSharedPtr<FJsonObject>& Params)
{
    // Optional point to re-centre the pasted group on; each actor keeps its offset from
    // the group's average location.
    const bool bRecentre = Params->HasField(TEXT("location"));
    const FVector GroupTarget = bRecentre ? mcp_common::ReadJsonVector(Params, TEXT("location")) : FVector::ZeroVector;

    UWorld* EditorWorld = UEAgentEditorCommandsImpl::ValidEditorWorld();
    if (EditorWorld == nullptr)
    {
        return UEAgentEditorCommandsImpl::NoWorldReply();
    }
    if (GEditor == nullptr)
    {
        return UEAgentEditorCommandsImpl::NoEditorReply();
    }

    FScopedTransaction PasteTransaction(FText::FromString(TEXT("Paste Actors")));

    // Paste the clipboard as it is (no duplicate pass, no offset, no hidden-actor warning);
    // the pasted actors are left as the editor selection.
    GEditor->edactPasteSelected(EditorWorld, /*bDuplicate=*/false, /*bOffsetLocations=*/false, /*bWarnIfHidden=*/false);
    const TArray<AActor*> Pasted = UEAgentEditorCommandsImpl::SelectedActorsIn(GEditor->GetSelectedActors(), EditorWorld);

    // Described where they landed, before any re-centring.
    const TArray<TSharedPtr<FJsonValue>> PastedJson = UEAgentEditorCommandsImpl::ActorsToJson(Pasted);

    if (bRecentre && Pasted.Num() > 0)
    {
        FVector Centre = FVector::ZeroVector;
        for (const AActor* Each : Pasted)
        {
            Centre += Each->GetActorLocation();
        }
        Centre /= Pasted.Num();
        const FVector Shift = GroupTarget - Centre;
        for (AActor* Each : Pasted)
        {
            Each->SetActorLocation(Each->GetActorLocation() + Shift);
        }
    }

    if (Pasted.Num() > 0)
    {
        GEditor->NoteSelectionChange();
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetArrayField(TEXT("pasted_actors"), PastedJson);
    Reply->SetNumberField(TEXT("pasted_count"), Pasted.Num());
    Reply->SetBoolField(TEXT("success"), Pasted.Num() > 0);
    return Reply;
}

// =====================================================================
// save_level -- THE PRODUCER of orphan worlds
//
// THE TRAP (reproducible, on a Content/ with no .umap at all):
//   a SaveAs to filename="/Game/..." can return true and WRITE NOTHING TO DISK.
//   What it does do is duplicate the world into a new UPackage IN MEMORY and
//   register it in the Asset Registry, leaving an ORPHAN WORLD that:
//     * is not the editor's current world,
//     * list_dirty_packages does NOT see (dirty_count:0 -- the orphan is not dirty),
//     * makes create_world_partition_map and new_level_from_template answer
//       already_exists:true with no file behind it,
//     * and KILLS load_level with the World Memory Leaks Fatal error.
//   A saved_filename that ECHOES the input always matches, so it can never reveal
//   that there is no file.
//
// THE THREE DEFENCES:
//   (1) ACTUALLY WRITE. Two pieces:
//       (a) FEditorFileUtils::SaveMap(World, Filename) wants a DISK file
//           (FileHelpers.h), but callers pass /Game/... paths (which is
//           what the OTHER overload, UEditorLoadingAndSavingUtils::SaveMap,
//           FileHelpers.h, wants). Either form is accepted here and
//           resolved to the disk path before calling.
//       (b) If the file still does not appear, the resulting package is rescued with
//           UEditorLoadingAndSavingUtils::SavePackages({Pkg}, bOnlyDirty=FALSE).
//           The `false` is mandatory and is the whole trap: the orphan package is NOT
//           marked dirty, so with bOnlyDirty=true nothing is written.
//   (2) success COMES FROM THE DISK. After saving, the file system is asked
//       (IFileManager::FileExists + FileSize) and `success` is derived from THAT, never
//       from the save call's bool nor from the echo of the input.
//   (3) HONEST ENVELOPE. MakeOutcomeReply, no inner `success`, and the input
//       filename duplicated under `requested` with AddRequestedEcho.
//
// And if after all that the file is not there, the response says so with the sentence
// the reader needs: an orphan world may have been left in memory, and load_level
// will REFUSE until the editor is restarted.
// =====================================================================

// Maps: save, load, engine template, which map is open

// MCP:save_level - Write the editor's current level (.umap) to disk; it REFUSES while PIE is running (the engine guard rejects the write anyway) and derives `success` from the file being on disk afterwards, never from the save call's own bool. WARNING: it also REFUSES when the target .umap is READ-ONLY on disk, which in an LFS repo means a `lockable` path you have not `git lfs lock`ed: that write cannot succeed, and answering over HEAD's still-untouched bytes would report success:true having written nothing. The refusal names the file and the fix (`git lfs lock <file>`) -- take the lock, do not retry. WARNING: on a file that ALREADY existed, `file_exists_on_disk` alone still cannot tell "I wrote it" from "it was already there": the field that proves the write is `file_mtime_changed`. It is deliberately NOT gating `success`, because a save with nothing dirty to write also leaves the mtime alone and that is not a failure. WARNING: with `filename` this is a SaveAs that DUPLICATES: the editor stays on the old map, the new one becomes a resident world (which then makes load_level refuse), and a SECOND save to the same filename opens a modal that freezes the editor -- treat filename as one-shot per name per session.
//   @pack levels
//   @param filename - Optional. A /Game asset path (e.g. /Game/Maps/Foo) or a disk .umap path; both accepted and resolved. Omit it to save the level in place -- that is the branch that just works. Passing one is a SaveAs: duplicate + resident world + the one-shot rule above.
//   @param save_as - Accepted for wire compatibility and IGNORED: the branch is chosen by whether `filename` was given. It comes back inside `requested` to mark it as your input, not a measurement.
//   @returns { success: bool, error?: string, data?: { saved_filename?: string, saved_package?: string, saved_disk_path?: string, level_name?: string, file_exists_on_disk: bool, file_size_bytes: int, file_existed_before: bool, file_mtime_changed: bool (the write itself -- see the read-only note above), save_call_returned: bool, rescued_with_save_packages: bool, pie_was_stopped: bool, orphan_world_risk?: bool, requested?: { filename?: string, save_as?: bool } } }

TSharedPtr<FJsonObject> HandleSaveLevel(const TSharedPtr<FJsonObject>& Params)
{
    // `save_as` is accepted but decides NOTHING: the branch is chosen by whether
    // `filename` was given. It is not read here on
    // purpose; it travels to the response as it arrived, under `requested` (AddRequestedEcho),
    // which is where it shows as an INPUT and not a reading.
    FString LevelFilename;
    Params->TryGetStringField(TEXT("filename"), LevelFilename);

    // The editor world -- the map that gets saved.
    UWorld* World = UEAgentEditorCommandsImpl::ValidEditorWorld();
    if (World == nullptr)
    {
        return UEAgentEditorCommandsImpl::NoWorldReply();
    }

    // PIE. The engine guard (Editor.AllowSavingAssetsDuringPIE=0) makes any save
    // with PIE active fail BEFORE attempting anything, without saying why, so the call is
    // refused up front and says so (see mcp_common::IsPieActive). The running PIE session is
    // left alone.
    //
    // WARNING: and in save_level refusing is worth double: with `filename` this command
    // DUPLICATES the world instead of renaming it and can leave it orphaned and resident in
    // memory -- from then on `load_level` is FATAL for the rest of the session.
    // It is single-use per name and session: spending it against the engine guard,
    // which will reject the write anyway, burns the name for nothing.
    const bool bPieActive = mcp_common::IsPieActive();
    if (bPieActive)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("REFUSED: a Play-in-Editor session is running. The engine guard ")
            TEXT("Editor.AllowSavingAssetsDuringPIE=0 refuses the write before attempting anything, ")
            TEXT("and save_level%s is single-use per name and session -- spending it on a guaranteed ")
            TEXT("refusal would burn the name for nothing. Stop PIE (stop_play_in_editor) and retry."),
            LevelFilename.IsEmpty() ? TEXT("") : TEXT(" with a filename")));
    }

    // Resolve the target: long package (/Game/...) AND disk path (.umap).
    // Either input form is accepted: FEditorFileUtils::SaveMap expects a DISK path, and
    // callers write /Game/...
    FString TargetPackageName;
    FString TargetDiskPath;
    if (!LevelFilename.IsEmpty())
    {
        if (FPackageName::IsValidLongPackageName(LevelFilename))
        {
            TargetPackageName = LevelFilename;
            FPackageName::TryConvertLongPackageNameToFilename(
                TargetPackageName, TargetDiskPath, FPackageName::GetMapPackageExtension());
        }
        else
        {
            TargetDiskPath = LevelFilename;
            FPackageName::TryConvertFilenameToLongPackageName(LevelFilename, TargetPackageName);
        }
    }

    // The SaveAs creates the target package and names the copied world's objects after it (and, for a level whose
    // actors live in their own packages, one package per actor). filename is not checked against the FName limit
    // before the handler runs (it may be a disk path), so a name past that limit is refused here: the save would
    // abort the editor.
    if (!TargetPackageName.IsEmpty())
    {
        if (const FString TooLong = AssetCommandHelpers::MapPathTooLongForExternalPackages(TargetPackageName); !TooLong.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TooLong);
        }
    }

    // Can the target be written? -- this command's blind spot.
    //
    // The `success` of the on-disk check below comes from a file EXISTING. That is honest compared with the
    // API's bool, but it does not tell "I wrote it" from "it was already there": a `.umap`
    // marked `lockable` in .gitattributes is left READ-ONLY on disk by git-lfs until
    // the lock is taken. Then the write fails, HEAD's file stays where it
    // was, and a file-exists check alone would answer `success:true` + `file_size_bytes` with
    // the OLD content and the mtime unchanged.
    //
    // It is REFUSED instead of attempted, for the same reason as the PIE guard
    // above: on a read-only file the write CANNOT succeed, and with `filename`
    // this command is single-use per name and session -- spending it against a
    // guaranteed failure burns the name for nothing.
    //
    // WARNING: the branch WITHOUT `filename` resolves TargetDiskPath after saving (below),
    // so here it has to be resolved first or the guard would look at nothing. Resolving it
    // twice is idempotent.
    //
    // AND THE GUARD HAS TO BE NARROW, or it forbids normal work. `FEditorFileUtils::SaveCurrentLevel`
    // (`FileHelpers.cpp`) adds the LEVEL package **only if it is dirty**, and separately
    // ALL the level's dirty OFPA packages. In World Partition the common case is
    // exactly that: you move an actor, ITS external package gets dirty and the `.umap` does not.
    // There a read-only `.umap` is irrelevant -- the external packages are not `lockable` --
    // and refusing would block legitimate work. => it only refuses when the `.umap` IS GOING
    // TO BE WRITTEN: with `filename` (a SaveAs always writes the target) or, without it, when
    // the level package is dirty or newly created.
    bool bLevelPackageWillBeWritten = !LevelFilename.IsEmpty();
    if (const ULevel* CurrentLevel = World->GetCurrentLevel())
    {
        if (const UPackage* LevelPackage = CurrentLevel->GetPackage())
        {
            if (TargetDiskPath.IsEmpty())
            {
                FPackageName::TryConvertLongPackageNameToFilename(
                    LevelPackage->GetName(), TargetDiskPath,
                    FPackageName::GetMapPackageExtension());
            }
            if (!bLevelPackageWillBeWritten)
            {
                bLevelPackageWillBeWritten = LevelPackage->IsDirty()
                    || LevelPackage->HasAnyPackageFlags(PKG_NewlyCreated);
            }
        }
    }

    // IFileManager and not FPlatformFileManager: it is the same one the on-disk check uses, so the
    // before and after measurements come from the same instrument.
    const bool bTargetExistedBefore =
        !TargetDiskPath.IsEmpty() && IFileManager::Get().FileExists(*TargetDiskPath);
    const bool bTargetReadOnlyBefore =
        bTargetExistedBefore && IFileManager::Get().IsReadOnly(*TargetDiskPath);
    const FDateTime MTimeBefore = bTargetExistedBefore
        ? IFileManager::Get().GetTimeStamp(*TargetDiskPath)
        : FDateTime::MinValue();

    if (bTargetReadOnlyBefore && bLevelPackageWillBeWritten)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("REFUSED: '%s' is READ-ONLY on disk and this save WOULD write it (%s), so the ")
            TEXT("write cannot succeed and this command would have answered success:true over ")
            TEXT("HEAD's bytes. In a Git LFS repo that is the LFS lock: paths marked `lockable` in ")
            TEXT(".gitattributes are restored read-only after every commit. Take the lock and ")
            TEXT("retry:  git lfs lock %s"),
            *TargetDiskPath,
            LevelFilename.IsEmpty() ? TEXT("the level package is dirty")
                                    : TEXT("a filename was given, so this is a SaveAs"),
            *TargetDiskPath));
    }

    // Reference: FEditorFileUtils::SaveMap, SaveCurrentLevel
    bool bSaveCallReturned = false;
    if (!LevelFilename.IsEmpty())
    {
        // FEditorFileUtils::SaveMap(World, Filename) wants a DISK path as Filename; that is
        // why the resolved TargetDiskPath is passed and not what arrived over the wire.
        bSaveCallReturned = FEditorFileUtils::SaveMap(
            World, TargetDiskPath.IsEmpty() ? LevelFilename : TargetDiskPath);
    }
    else
    {
        // Normal save in place (no filename), through FEditorFileUtils::SaveCurrentLevel():
        // the branch that DOES work. Nothing is touched beyond checking on disk what it left
        // behind, so the target is read back off the current level's package.
        bSaveCallReturned = FEditorFileUtils::SaveCurrentLevel();

        const ULevel* SavedLevel = World->GetCurrentLevel();
        if (const UPackage* SavedPackage = SavedLevel ? SavedLevel->GetOutermost() : nullptr)
        {
            TargetPackageName = SavedPackage->GetName();
            FPackageName::TryConvertLongPackageNameToFilename(
                TargetPackageName, TargetDiskPath, FPackageName::GetMapPackageExtension());
        }
    }

    // Is the file there? -- the only valid oracle of this command.
    auto FileIsOnDisk = [&TargetDiskPath](int64& OutSize) -> bool
    {
        OutSize = 0;
        if (TargetDiskPath.IsEmpty())
        {
            return false;
        }
        if (!IFileManager::Get().FileExists(*TargetDiskPath))
        {
            return false;
        }
        const int64 Size = IFileManager::Get().FileSize(*TargetDiskPath);
        OutSize = (Size > 0) ? Size : 0;
        return Size > 0;
    };

    int64 FileSizeBytes = 0;
    bool bOnDisk = FileIsOnDisk(FileSizeBytes);

    // Rescue the orphan. If there is no file, the duplicated world is still alive in
    // memory inside its UPackage: it is located and written with bOnlyDirty=FALSE.
    // With true nothing would be written -- the orphan package is not marked dirty, and
    // that is where the whole save was lost without a single warning.
    bool bRescuedWithSavePackages = false;
    if (!bOnDisk && !TargetPackageName.IsEmpty())
    {
        if (UPackage* OrphanPackage = FindPackage(nullptr, *TargetPackageName))
        {
            TArray<UPackage*> PackagesToSave;
            PackagesToSave.Add(OrphanPackage);
            UE_LOG(LogTemp, Warning,
                TEXT("[MCP] save_level: nothing on disk at '%s' after the save call. Rescuing the in-memory package '%s' with SavePackages(bOnlyDirty=false)."),
                *TargetDiskPath, *TargetPackageName);
            bRescuedWithSavePackages = UEditorLoadingAndSavingUtils::SavePackages(
                PackagesToSave, /*bOnlyDirty=*/false);
            bOnDisk = FileIsOnDisk(FileSizeBytes);
        }
    }

    // Build the response -- success DERIVED FROM THE DISK, not from the echo nor the bool.
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("saved_package"), TargetPackageName);
    Data->SetStringField(TEXT("saved_disk_path"), TargetDiskPath);
    Data->SetStringField(TEXT("level_name"), World->GetName());
    Data->SetBoolField(TEXT("file_exists_on_disk"), bOnDisk);
    Data->SetNumberField(TEXT("file_size_bytes"), static_cast<double>(FileSizeBytes));
    // The pair that separates "I wrote it" from "it was already there" -- which
    // `file_exists_on_disk` does NOT tell apart on its own (the false success the
    // writability check above closes): on a file that already existed, what proves the write
    // is the mtime MOVING.
    // WARNING: it does not gate `success` on purpose: a save with nothing dirty to
    // write does not move the mtime either, and that is not a failure. It is emitted so the
    // reader can tell them apart; the case that would lie (read-only) is refused above.
    // And `target_read_only` is ALWAYS emitted, because the guard is narrow on purpose: in
    // the World Partition case -- only dirty OFPA packages -- the save goes ahead with the
    // `.umap` read-only and nothing refused, and the reader has to be able to see that.
    Data->SetBoolField(TEXT("target_read_only"), bTargetReadOnlyBefore);
    Data->SetBoolField(TEXT("file_existed_before"), bTargetExistedBefore);
    Data->SetBoolField(TEXT("file_mtime_changed"),
        bOnDisk && (!bTargetExistedBefore
                    || IFileManager::Get().GetTimeStamp(*TargetDiskPath) != MTimeBefore));
    Data->SetBoolField(TEXT("save_call_returned"), bSaveCallReturned);
    Data->SetBoolField(TEXT("rescued_with_save_packages"), bRescuedWithSavePackages);
    // Always false: with PIE the call was refused above, so no session is ever stopped. The
    // field stays because clients read it.
    Data->SetBoolField(TEXT("pie_was_stopped"), false);
    if (bOnDisk)
    {
        // This field is only emitted when there is a file behind it, and it holds the
        // verified disk path -- not the echo of the input.
        Data->SetStringField(TEXT("saved_filename"), TargetDiskPath);
    }
    else
    {
        Data->SetBoolField(TEXT("orphan_world_risk"), true);
    }
    // The filename/save_as the caller sent, marked as INPUT and not as a reading.
    mcp_common::AddRequestedEcho(Data, Params, { TEXT("filename"), TEXT("save_as") });

    FString Error;
    if (!bOnDisk)
    {
        Error = FString::Printf(
            TEXT("save_level did NOT produce a file on disk (checked '%s'; save call returned %s, rescue via SavePackages(bOnlyDirty=false) %s). ")
            TEXT("The duplicated world is most likely still ORPHANED IN MEMORY: list_dirty_packages will not show it (an orphan package is not marked dirty), ")
            TEXT("create_world_partition_map and new_level_from_template will answer already_exists:true with no file behind it, ")
            TEXT("and load_level WILL REFUSE every map load until the editor is restarted (a resident world makes LoadMap a Fatal error). ")
            TEXT("PIE was NOT active (checked before saving), so the engine's ")
            TEXT("Editor.AllowSavingAssetsDuringPIE guard is NOT the explanation here. ")
            TEXT("Verify with the filesystem, never with this command's saved_filename."),
            TargetDiskPath.IsEmpty() ? TEXT("<could not resolve a disk path>") : *TargetDiskPath,
            bSaveCallReturned ? TEXT("true") : TEXT("false"),
            bRescuedWithSavePackages ? TEXT("also returned true") : TEXT("did not help"));
    }

    return mcp_common::MakeOutcomeReply(Data, bOnDisk, Error);
}

namespace
{
    // =====================================================================
    // RESIDENT-WORLD PREFLIGHT FOR load_level
    //
    // WHY THIS EXISTS:
    //   FEditorFileUtils::LoadMap() eventually runs UEditorEngine::EditorDestroyWorld,
    //   which ends in UEditorEngine::CheckForWorldGCLeaks (EditorServer.cpp). If
    //   any UWorld survives the cleanse, that function does
    //       UE_LOGF(LogEditorServer, Fatal, "World Memory Leaks: %d leaks objects and packages")
    //   -- Fatal, not Error, because Editor.CheckForWorldGCLeaksAreFatal defaults to true
    //   (EditorServer.cpp). A Fatal error is not a return value: the process dies,
    //   the session is lost, and with it every in-memory edit.
    //
    //   Known producers of resident worlds in this plugin:
    //     * create_world_partition_map -- builds an EWorldType::Inactive UWorld inside a
    //       /Game package with bInformEngineOfWorld=false and never releases it. It
    //       survives repeated CollectGarbage and unload_packages() (which returns true
    //       without unloading anything). No way to free it from a running editor is known.
    //     * save_level(save_as=true) -- duplicates the world into a fresh UPackage and
    //       registers it in the Asset Registry, leaving an orphan world/package.
    //
    //   !! It does NOT matter that the resident world IS the map you are about to load, nor that it
    //   has a real .umap on disk. The leak check counts it just the same. "It's my own
    //   map, not a ghost, so loading is safe" is FALSE and costs the editor.
    //
    //   The only remedy is a fresh editor process (a new session is born without
    //   the resident world), or opening the map through the editor UI.
    //
    // WHAT WE EXCLUDE, AND WHY (the guard must not cry wolf on a healthy editor):
    //   * The world the editor is currently editing -- it is the one being replaced.
    //   * PIE worlds -- PIE teardown owns them and they have their own path.
    //   * EditorPreview / GamePreview -- every open asset editor, thumbnail render and
    //     preview scene creates one. The engine's own leak check exempts them explicitly
    //     (bIsPersistantWorldType), so they can never be the cause of the Fatal.
    //   * Worlds in the transient package -- never persisted, never the leak.
    //   * Sublevels / level instances of the current editor world (their PersistentLevel
    //     ->OwningWorld points at it). Streamed sublevels adopt the persistent world's
    //     WorldType (LevelStreaming.cpp), so without this rule every map with
    //     sublevels would look like a leak.
    //
    //   EWorldType::Inactive is deliberately NOT excluded even though the engine's own
    //   check exempts it: create_world_partition_map produces exactly an Inactive
    //   world. This guard is intentionally WIDER than the engine's
    //   check -- refusing a load that would have worked costs one call; guessing wrong
    //   the other way kills the editor. `force` exists for that call.
    // =====================================================================
    struct FMcpResidentWorld
    {
        FString PackageName;
        FString WorldName;
        FString WorldTypeName;
        bool bHasWorldContext = false;
        bool bIsLoadTarget = false;
    };

    // The engine ships LexToString(EWorldType::Type) (EngineTypes.h) and it resolves
    // fine -- there is no competing generic LexToString template in Core. We spell the
    // switch out anyway: it is one dependency less, and it names the 8 world types
    // explicitly instead of collapsing anything unexpected into "Unknown" silently.
    const TCHAR* McpWorldTypeToString(EWorldType::Type WorldType)
    {
        switch (WorldType)
        {
        case EWorldType::None:          return TEXT("None");
        case EWorldType::Game:          return TEXT("Game");
        case EWorldType::Editor:        return TEXT("Editor");
        case EWorldType::PIE:           return TEXT("PIE");
        case EWorldType::EditorPreview: return TEXT("EditorPreview");
        case EWorldType::GamePreview:   return TEXT("GamePreview");
        case EWorldType::GameRPC:       return TEXT("GameRPC");
        case EWorldType::Inactive:      return TEXT("Inactive");
        default:                        return TEXT("Unknown");
        }
    }

    // Enumerate the UWorlds still resident in memory that could turn the next LoadMap
    // into a Fatal error. Read-only: touches nothing, allocates nothing in the engine.
    void McpCollectResidentWorlds(const UWorld* CurrentEditorWorld,
                                  const FString& TargetPackageName,
                                  TArray<FMcpResidentWorld>& OutResidents)
    {
        for (TObjectIterator<UWorld> It; It; ++It)
        {
            UWorld* World = *It;
            if (!World || !IsValid(World) || World == CurrentEditorWorld)
            {
                continue;
            }

            const EWorldType::Type WorldType = World->WorldType;
            if (WorldType == EWorldType::PIE
                || WorldType == EWorldType::EditorPreview
                || WorldType == EWorldType::GamePreview)
            {
                continue;
            }

            UPackage* Package = World->GetOutermost();
            if (!Package || Package == GetTransientPackage())
            {
                continue;
            }

            // Sublevel / level instance hanging off the world we are leaving: it is torn
            // down with its owner, so it is not an orphan.
            if (CurrentEditorWorld && World->PersistentLevel
                && World->PersistentLevel->OwningWorld.Get() == CurrentEditorWorld)
            {
                continue;
            }

            FMcpResidentWorld Entry;
            Entry.PackageName = Package->GetName();
            Entry.WorldName = World->GetName();
            Entry.WorldTypeName = McpWorldTypeToString(WorldType);
            Entry.bHasWorldContext = (GEngine != nullptr) && (GEngine->GetWorldContextFromWorld(World) != nullptr);
            Entry.bIsLoadTarget = !TargetPackageName.IsEmpty() && (Entry.PackageName == TargetPackageName);
            OutResidents.Add(MoveTemp(Entry));
        }
    }

    // JSON view of the resident set, so callers never have to parse the prose message.
    TArray<TSharedPtr<FJsonValue>> McpResidentWorldsToJson(const TArray<FMcpResidentWorld>& Residents)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Reserve(Residents.Num());
        for (const FMcpResidentWorld& Resident : Residents)
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("package"), Resident.PackageName);
            Obj->SetStringField(TEXT("world_name"), Resident.WorldName);
            Obj->SetStringField(TEXT("world_type"), Resident.WorldTypeName);
            Obj->SetBoolField(TEXT("has_world_context"), Resident.bHasWorldContext);
            Obj->SetBoolField(TEXT("is_load_target"), Resident.bIsLoadTarget);
            Out.Add(MakeShared<FJsonValueObject>(Obj));
        }
        return Out;
    }

    // THE guard, shared by every handler that is about to call FEditorFileUtils::LoadMap
    // (today: load_level, which also loads the engine template map, so it carries exactly the
    // same Fatal error whatever map it opens).
    //
    // Returns nullptr when the load may proceed (nothing resident, or bForce). Otherwise
    // returns the refusal response, ready to be returned as-is. OutResidents is filled in
    // both cases so the caller can report what it stepped over when forcing.
    TSharedPtr<FJsonObject> McpPreflightResidentWorlds(const TCHAR* CommandName,
                                                      const FString& RequestedFilename,
                                                      const FString& TargetPackageName,
                                                      bool bForce,
                                                      TArray<FMcpResidentWorld>& OutResidents)
    {
        const UWorld* CurrentEditorWorld = mcp_common::GetEditorWorld();
        OutResidents.Reset();
        McpCollectResidentWorlds(CurrentEditorWorld, TargetPackageName, OutResidents);

        if (OutResidents.Num() == 0 || bForce)
        {
            return nullptr;
        }

        FString PackageList;
        bool bAnyIsLoadTarget = false;
        for (const FMcpResidentWorld& Resident : OutResidents)
        {
            if (!PackageList.IsEmpty())
            {
                PackageList += TEXT(", ");
            }
            PackageList += FString::Printf(TEXT("%s (%s)"), *Resident.PackageName, *Resident.WorldTypeName);
            bAnyIsLoadTarget = bAnyIsLoadTarget || Resident.bIsLoadTarget;
        }

        const FString Remedy = TEXT("RESTART THE EDITOR -- a fresh session is born without the resident world -- or open the map through the editor UI. CollectGarbage and EditorLoadingAndSavingUtils.unload_packages() do NOT release it (unload_packages returns true having unloaded nothing).");

        const FString Message = FString::Printf(
            TEXT("%s REFUSED: %d UWorld(s) other than the current editor world are still resident in memory [%s]. ")
            TEXT("Loading a map while they are alive makes the engine raise a Fatal error ")
            TEXT("(\"World Memory Leaks: N leaks objects and packages\", EditorServer.cpp CheckForWorldGCLeaks) ")
            TEXT("which KILLS THE EDITOR PROCESS: the session and every unsaved in-memory edit are lost. ")
            TEXT("%s")
            TEXT("Remedy: %s ")
            TEXT("Pass force=true to attempt the load anyway (this probably kills the editor)."),
            CommandName,
            OutResidents.Num(),
            *PackageList,
            bAnyIsLoadTarget
                ? TEXT("NOTE: one of the resident worlds IS the map you asked to load, with a real .umap on disk. That does NOT make it safe -- the engine's leak check counts it just the same. ")
                : TEXT(""),
            *Remedy);

        TSharedPtr<FJsonObject> Refusal = mcp_common::MakeErrorReply(Message);
        Refusal->SetBoolField(TEXT("refused"), true);
        Refusal->SetBoolField(TEXT("load_attempted"), false);
        Refusal->SetStringField(TEXT("loaded_filename"), FString());
        Refusal->SetStringField(TEXT("requested_filename"), RequestedFilename);
        Refusal->SetStringField(TEXT("target_package"), TargetPackageName);
        Refusal->SetNumberField(TEXT("resident_world_count"), OutResidents.Num());
        Refusal->SetArrayField(TEXT("resident_worlds"), McpResidentWorldsToJson(OutResidents));
        Refusal->SetBoolField(TEXT("resident_world_is_load_target"), bAnyIsLoadTarget);
        Refusal->SetStringField(TEXT("remedy"), Remedy);
        return Refusal;
    }

    // Shared warning log for the force path. Keeps the "if the editor dies here it is the
    // engine, not a plugin crash" note in ONE place for every LoadMap caller.
    void McpLogForcedPastResidentWorlds(const TCHAR* CommandName, int32 ResidentCount)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[MCP] %s force=true with %d resident world(s): calling FEditorFileUtils::LoadMap anyway. If the editor dies here, that is the engine's World Memory Leaks Fatal error, not a plugin crash."),
            CommandName, ResidentCount);
    }

    // What McpPrepareMapLoad found before a LoadMap.
    struct FMcpMapLoadPreflight
    {
        int32 PurgedScriptRefRoots = 0;
        TArray<FMcpResidentWorld> ResidentWorlds;
        bool bForcedPastResidentWorlds = false;
    };

    // What runs before every FEditorFileUtils::LoadMap, in this order on purpose: release the script
    // references to the worlds the load destroys (see McpCommonUtils.h) and, outside PIE, collect
    // garbage, so a world only a Python global kept alive is gone; then the resident-world
    // preflight, since a resident world turns LoadMap into an engine Fatal error. Returns the
    // preflight's refusal, or nullptr when the load may proceed (a forced load is logged).
    TSharedPtr<FJsonObject> McpPrepareMapLoad(const TCHAR* CommandName,
                                              const FString& RequestedFilename,
                                              const FString& TargetPackageName,
                                              bool bForce,
                                              FMcpMapLoadPreflight& OutPreflight)
    {
        OutPreflight.PurgedScriptRefRoots = mcp_common::PurgeWorldsBeforeLoadMap(TargetPackageName);
        if (OutPreflight.PurgedScriptRefRoots > 0 && (GEditor == nullptr || GEditor->PlayWorld == nullptr))
        {
            CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, /*bPerformFullPurge=*/true);
        }

        if (TSharedPtr<FJsonObject> Refusal = McpPreflightResidentWorlds(
                CommandName, RequestedFilename, TargetPackageName, bForce, OutPreflight.ResidentWorlds))
        {
            return Refusal;
        }

        OutPreflight.bForcedPastResidentWorlds = bForce && (OutPreflight.ResidentWorlds.Num() > 0);
        if (OutPreflight.bForcedPastResidentWorlds)
        {
            McpLogForcedPastResidentWorlds(CommandName, OutPreflight.ResidentWorlds.Num());
        }
        return nullptr;
    }

    // The preflight's findings, as the fields every LoadMap reply carries.
    void McpAddMapLoadPreflightFields(const TSharedPtr<FJsonObject>& Data, const FMcpMapLoadPreflight& Preflight)
    {
        Data->SetNumberField(TEXT("resident_world_count"), Preflight.ResidentWorlds.Num());
        Data->SetBoolField(TEXT("forced_past_resident_worlds"), Preflight.bForcedPastResidentWorlds);
        // How many roots (world or package) were released from script references before counting
        // residents. >0 with resident_world_count 0 is a world that would have leaked, released so
        // the load can go ahead.
        Data->SetNumberField(TEXT("script_ref_roots_purged"), Preflight.PurgedScriptRefRoots);
        if (Preflight.ResidentWorlds.Num() > 0)
        {
            Data->SetArrayField(TEXT("resident_worlds"), McpResidentWorldsToJson(Preflight.ResidentWorlds));
        }
    }
}

// MCP:load_level - Open a map in the editor (by /Game or /Engine package path, or disk .umap), replacing the current world; default_template=true opens the engine's default template level instead (/Engine/Maps/Templates/Template_Default, ~45 actors: floor, PlayerStart, sky and lights), after which a save_level without `filename` would write over the ENGINE's own map (is_engine_template and warning say so). Also the repair for actors that VANISHED from a world in memory after compile_blueprint: reloading the same map brings the placed OFPA instances back with their per-instance config -- never re-place them. REFUSES up front when any other UWorld is still resident in memory, because the engine's world-leak check turns that into a Fatal error that kills the editor process and every unsaved edit with it; the refusal lists the resident worlds and the remedy. Run list_dirty_packages first: a reload discards unsaved work.
//   @pack levels
//   @param filename - Disk path to a .umap, OR a long package path (e.g. /Game/Maps/Foo) resolved to the on-disk map file. Required unless default_template is true.
//   @param default_template - Optional bool (default false): open the engine's default template level (/Engine/Maps/Templates/Template_Default) without naming it in filename; giving both is an error.
//   @param load_as_template - When true, loads the level as a template; defaults to false
//   @param force - Skip the resident-world preflight and call LoadMap anyway. With resident worlds alive this PROBABLY KILLS THE EDITOR: the engine raises a Fatal error ("World Memory Leaks"), the process dies and every unsaved in-memory edit is lost. Defaults to false.
//   @returns { success: bool, error?: string, refused?: bool, load_attempted?: bool, resident_world_count?: int, resident_worlds?: [{ package: string, world_name: string, world_type: string, has_world_context: bool, is_load_target: bool }], remedy?: string, data?: { loaded_filename: string, target_package: string, default_template: bool, level_name?: string, world_name?: string, loaded_package?: string, is_engine_template?: bool, warning?: string (when the loaded map belongs to the engine), resident_world_count: int, forced_past_resident_worlds: bool, resident_worlds?: [...] } }

TSharedPtr<FJsonObject> HandleLoadLevel(const TSharedPtr<FJsonObject>& Params)
{
    // The map to open: filename, or the engine's default template level (what new_level opened).
    FString LevelFilename;
    const bool bFilenameGiven = Params->TryGetStringField(TEXT("filename"), LevelFilename);
    bool bDefaultTemplate = false;
    Params->TryGetBoolField(TEXT("default_template"), bDefaultTemplate);
    if (bDefaultTemplate)
    {
        if (bFilenameGiven && !LevelFilename.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TEXT("Pass filename OR default_template=true, not both; nothing was loaded"));
        }
        LevelFilename = TEXT("/Engine/Maps/Templates/Template_Default");
    }
    else if (!bFilenameGiven)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'filename' (or pass default_template=true)"));
    }
    if (LevelFilename.IsEmpty())
    {
        return UEAgentEditorCommandsImpl::EmptyParamReply(TEXT("filename"));
    }

    // Accept a /Game asset path (e.g. "/Game/Maps/Foo"), not just a disk filename:
    // resolve a valid long package name to its on-disk .umap path. Mirrors the
    // natural round-trip for maps referenced elsewhere as package paths.
    // TargetPackageName is kept so the preflight below can tell the caller whether a
    // resident world IS the map being loaded -- which is NOT a reason to proceed.
    FString TargetPackageName;
    if (FPackageName::IsValidLongPackageName(LevelFilename))
    {
        TargetPackageName = LevelFilename;
        FString MapDiskPath;
        if (FPackageName::TryConvertLongPackageNameToFilename(
                LevelFilename, MapDiskPath, FPackageName::GetMapPackageExtension()))
        {
            LevelFilename = MapDiskPath;
        }
    }
    else
    {
        FPackageName::TryConvertFilenameToLongPackageName(LevelFilename, TargetPackageName);
    }

    // It must name a map file (FEditorFileUtils::IsValidMapFilename says why not)
    FText InvalidReason;
    if (!FEditorFileUtils::IsValidMapFilename(LevelFilename, InvalidReason))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid map filename: %s"), *InvalidReason.ToString()));
    }

    // filename is not checked against the FName limit before the handler runs (it may be a disk path), and the load
    // makes an FName of the map's package name. A map is also refused at a path too long for the objects the editor
    // names after it: it would abort as soon as an actor or an actor folder is added to the world.
    if (!TargetPackageName.IsEmpty())
    {
        if (const FString TooLong = AssetCommandHelpers::MapPathTooLongForExternalPackages(TargetPackageName); !TooLong.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TooLong);
        }
    }

    bool bForce = false;
    Params->TryGetBoolField(TEXT("force"), bForce);

    // Script references released, then the resident-world preflight -- the only thing
    // separating an honest error from a dead process.
    FMcpMapLoadPreflight Preflight;
    if (TSharedPtr<FJsonObject> Refusal = McpPrepareMapLoad(
            TEXT("load_level"), LevelFilename, TargetPackageName, bForce, Preflight))
    {
        return Refusal;
    }

    // Load the level: FEditorFileUtils::LoadMap(Filename, LoadAsTemplate, bShowProgress)
    bool bAsTemplate = false;
    Params->TryGetBoolField(TEXT("load_as_template"), bAsTemplate);

    // With the progress dialog, like the editor's own File > Open.
    const bool bSuccess = FEditorFileUtils::LoadMap(LevelFilename, bAsTemplate, /*bShowProgress=*/true);

    // The world the editor is on now (only read after a successful load)
    UWorld* World = bSuccess ? mcp_common::GetEditorWorld() : nullptr;

    // The envelope's `success` is derived by MakeOutcomeReply from LoadMap's real
    // bSuccess; the payload carries no inner `success` of its own to compete with.
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("loaded_filename"), LevelFilename);
    Data->SetStringField(TEXT("target_package"), TargetPackageName);
    Data->SetBoolField(TEXT("default_template"), bDefaultTemplate);
    McpAddMapLoadPreflightFields(Data, Preflight);
    if (bSuccess && World)
    {
        Data->SetStringField(TEXT("level_name"), World->GetName());
        Data->SetStringField(TEXT("world_name"), World->GetName());

        // The REAL package the editor ended up on, read from the world, not assumed. An /Engine map (the
        // default template) is one a save_level without `filename` would write over.
        const UPackage* WorldPackage = World->GetOutermost();
        const FString LoadedPackage = WorldPackage ? WorldPackage->GetName() : FString();
        const bool bIsEngineTemplate = LoadedPackage.StartsWith(TEXT("/Engine/"));
        Data->SetStringField(TEXT("loaded_package"), LoadedPackage);
        Data->SetBoolField(TEXT("is_engine_template"), bIsEngineTemplate);
        if (bIsEngineTemplate)
        {
            Data->SetStringField(TEXT("warning"), FString::Printf(
                TEXT("The editor is now sitting on %s, a map that belongs to the ENGINE. A save_level WITHOUT `filename` ")
                TEXT("from here writes over that map's own .umap; save your level explicitly with ")
                TEXT("save_level(filename=\"/Game/.../YourMap\")."), *LoadedPackage));
        }
    }

    return mcp_common::MakeOutcomeReply(
        Data,
        bSuccess,
        bSuccess ? FString() : TEXT("Failed to load level. Check if file exists and is a valid Unreal map file."));
}

// MCP:get_current_level - Which map a world is sitting on: level_name, its package_name (/Game/... or /Engine/...), the resolved disk filename, whether the package is dirty, and a live actor count. package_name is the discriminator that catches the classic mistake -- if it starts with /Engine/ you are on an engine template (what load_level with default_template=true leaves behind) and a save_level without `filename` would overwrite the ENGINE's own map. Defaults to the EDITOR world; it also takes the standard `world` selector, so during PIE you can ask the play world ('pie'/'server'/'client'/'pie:N') instead of being told about the authoring map and having no way to know it.
//   @pack levels
//   @param world - Optional world selector: absent/'editor' (default -- the authoring map even while PIE runs) | 'pie' (ambiguous: PlayWorld, else the first PIE context) | 'game' (alias of 'pie') | 'server' (the PIE world with authority) | 'client' | 'pie:N'. Case-insensitive; an unrecognised value is a clean error listing the worlds present, never a silent fallback to the editor world.
//   @returns { level_name: string, package_name: string (the /Game or /Engine path -- read this one), filename: string, is_dirty: bool, actor_count: int, world: string (the normalized selector that resolved), world_name: string, world_package: string (the UEDPIE_N prefix lives HERE, not in world_name), world_type: string ("Editor" | "PIE" | ...), net_mode: string, pie_instance: int }

TSharedPtr<FJsonObject> HandleGetCurrentLevel(const TSharedPtr<FJsonObject>& Params)
{
    // With `world` absent this is the editor world -- the authoring map, even while PIE
    // runs. Another world CAN be asked for, and every reply says which one answered.
    FString SelectorUsed;
    FString ResolveFailure;
    UWorld* World = ResolveEditorOrPieWorld(Params, SelectorUsed, ResolveFailure);
    if (!World)
    {
        return mcp_common::MakeErrorReply(ResolveFailure);
    }

    FString PackagePath;
    FString DiskFile;
    bool bUnsaved = false;
    const ULevel* Current = World->GetCurrentLevel();
    if (const UPackage* Owner = (Current != nullptr) ? Current->GetOutermost() : nullptr)
    {
        PackagePath = Owner->GetName();
        bUnsaved = Owner->IsDirty();
        // Stays empty for a package that has never been written to disk.
        FString OnDisk;
        if (FPackageName::DoesPackageExist(PackagePath, &OnDisk))
        {
            DiskFile = OnDisk;
        }
    }

    int32 LiveActors = 0;
    for (TActorIterator<AActor> Cursor(World); Cursor; ++Cursor)
    {
        ++LiveActors;
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("level_name"), World->GetName());
    Reply->SetStringField(TEXT("package_name"), PackagePath);
    Reply->SetStringField(TEXT("filename"), DiskFile);
    Reply->SetBoolField(TEXT("is_dirty"), bUnsaved);
    Reply->SetNumberField(TEXT("actor_count"), LiveActors);
    Reply->SetStringField(TEXT("world"), SelectorUsed);
    // world_name / world_package / world_type / net_mode / pie_instance. On the default
    // path world_type reads "Editor"; elsewhere it names the real type ("PIE", "Game", ...).
    mcp_common::AddWorldEvidence(Reply, World);
    return Reply;
}

// Many actors in one undoable step

// MCP:set_actor_transforms - Move/rotate/scale MANY actors in a single undoable transaction, each with its own optional location/rotation/scale, reporting success_count, failure_count and the names that did not resolve. Cheaper and atomically undoable versus N set_actor_transform calls. Calls Modify() per actor so their OFPA packages dirty and save_level actually writes them. Takes the same `world` selector as set_actor_transform, so it reaches a live PIE world too. WARNING: resolves the INTERNAL name only (no editor labels, unlike set_actor_transform); a name it cannot find is counted in failure_count, not raised as an error -- read that number.
//   @retired execute_batch - one set_actor_transform per actor, with transaction=true for one undo step
//   @param actor_transforms - Array of objects {actor_name: internal GetName; location?: [x,y,z] array only; rotation?: [pitch,yaw,roll] or the safer {pitch,yaw,roll}; scale?: [x,y,z] array only}. Omitted components keep their current value; an entry with no actor_name just increments failure_count, and a malformed vector silently reads as zero.
//   @param world - Optional: "editor" (default) | "pie" | "server" | "client" | "pie:N". The response stamps which world answered.
//   @returns { success_count: int, failure_count: int (unresolved or malformed entries -- check it), updated_actors: [actor JSON re-read after the write], failed_actors?: [string], world_name, world_package, ... }

TSharedPtr<FJsonObject> HandleBatchSetActorTransform(const TSharedPtr<FJsonObject>& Params)
{
    const TArray<TSharedPtr<FJsonValue>>* Requests = nullptr;
    if (!Params->TryGetArrayField(TEXT("actor_transforms"), Requests) || !Requests || Requests->Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: actor_transforms (array of {actor_name, location?, rotation?, scale?})"));
    }

    // The same `world` as set_actor_transform (see the note there): pinned to the editor,
    // the batch could not reach a PIE actor either, and its "not found" would read as
    // "does not exist".
    FString WorldError;
    UWorld* World = mcp_common::ResolveWorldFromParams(Params, WorldError);
    if (World == nullptr || !World->IsValidLowLevel())
    {
        return mcp_common::MakeErrorReply(WorldError.IsEmpty() ? TEXT("No valid world found") : WorldError);
    }

    // The whole batch is a single undo step.
    FScopedTransaction BatchTransaction(FText::FromString(TEXT("Batch Set Actor Transform")));

    const TArray<AActor*> Pool = UEAgentEditorCommandsImpl::SnapshotActors(World);
    TArray<TSharedPtr<FJsonValue>> UpdatedJson;
    TArray<FString> Unresolved;
    int32 Failures = 0;
    for (const TSharedPtr<FJsonValue>& Item : *Requests)
    {
        // Each entry must be an object with a non-empty actor_name; anything else is
        // only counted as a failure.
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        FString WantedName;
        const bool bWellFormed = Item->TryGetObject(Entry) && Entry != nullptr && Entry->IsValid()
            && (*Entry)->TryGetStringField(TEXT("actor_name"), WantedName) && !WantedName.IsEmpty();
        if (!bWellFormed)
        {
            ++Failures;
            continue;
        }

        AActor* Target = UEAgentEditorCommandsImpl::PickByObjectName(Pool, WantedName);
        if (Target == nullptr)
        {
            Unresolved.Add(WantedName);
            ++Failures;
            continue;
        }

        // Modify() before the write, so the OFPA package is dirtied (see set_actor_transform).
        Target->Modify();
        FTransform Updated = Target->GetTransform();
        UEAgentEditorCommandsImpl::OverlayTransformFields(*Entry, Updated);
        Target->SetActorTransform(Updated);
        UpdatedJson.Add(mcp_common::DescribeActorValue(Target));
    }

    if (GEditor != nullptr && UpdatedJson.Num() > 0)
    {
        GEditor->NoteSelectionChange();
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetNumberField(TEXT("success_count"), UpdatedJson.Num());
    Reply->SetNumberField(TEXT("failure_count"), Failures);
    Reply->SetArrayField(TEXT("updated_actors"), UpdatedJson);
    if (Unresolved.Num() > 0)
    {
        Reply->SetArrayField(TEXT("failed_actors"), mcp_common::StringsToJson(Unresolved));
    }
    // Which world was written: a high failure_count with world='editor' over PIE names is
    // a different diagnosis from "those actors do not exist".
    mcp_common::AddWorldEvidence(Reply, World);
    return Reply;
}

// MCP:delete_actors - Delete several editor-world actors at once through the editor's own delete path, in ONE undoable transaction (a single undo brings them all back), returning the count and the JSON of each actor captured before deletion. WARNING: matches INTERNAL names only -- editor labels do not resolve -- names it cannot find are skipped silently, so compare deleted_count against how many you asked for; and if NONE resolve it errors with "No actors found to delete". Editor world only, and the removal is in memory until save_level writes the .umap.
//   @param actor_names - Array of internal actor names (GetName), case-insensitive whole-string. Unmatched entries are dropped without an error; resolve labels first with find_actors_by_name.
//   @returns { deleted_count: int (compare against your input length), deleted_actors: [actor JSON captured before the delete] }

TSharedPtr<FJsonObject> HandleBatchDeleteActors(const TSharedPtr<FJsonObject>& Params)
{
    const TArray<TSharedPtr<FJsonValue>>* DoomedNames = nullptr;
    if (!Params->TryGetArrayField(TEXT("actor_names"), DoomedNames) || !DoomedNames || DoomedNames->Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: actor_names (array)"));
    }

    UWorld* EditorWorld = UEAgentEditorCommandsImpl::ValidEditorWorld();
    if (EditorWorld == nullptr)
    {
        return UEAgentEditorCommandsImpl::NoWorldReply();
    }
    if (GEditor == nullptr)
    {
        return UEAgentEditorCommandsImpl::NoEditorReply();
    }

    // One transaction, so a single undo brings every actor back.
    FScopedTransaction BatchTransaction(FText::FromString(TEXT("Batch Delete Actors")));

    // Names that do not resolve are skipped without an error; deleted_count tells.
    const TArray<AActor*> Doomed = UEAgentEditorCommandsImpl::ResolveNameList(
        *DoomedNames, UEAgentEditorCommandsImpl::SnapshotActors(EditorWorld), nullptr);
    if (Doomed.Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("No actors found to delete"));
    }

    // Described while they still exist.
    const TArray<TSharedPtr<FJsonValue>> DoomedJson = UEAgentEditorCommandsImpl::ActorsToJson(Doomed);

    // The editor delete works on the selection: select exactly these, then delete them.
    UEAgentEditorCommandsImpl::ClearEditorSelection();
    for (AActor* Each : Doomed)
    {
        UEAgentEditorCommandsImpl::AddToEditorSelection(Each, /*bNotify=*/false);
    }
    GEditor->edactDeleteSelected(EditorWorld);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetNumberField(TEXT("deleted_count"), Doomed.Num());
    Reply->SetArrayField(TEXT("deleted_actors"), DoomedJson);
    return Reply;
}

// Grounding (trace or heightmap) and lining actors up

// MCP:snap_actor_to_ground - Drop ONE editor-world actor straight down until its bounds bottom rests on whatever is below -- meshes, BSP or the landscape. It line-traces first and, because headless traces routinely MISS the landscape, falls back to sampling the heightmap so the actor still lands; sampled_from tells you which path answered. Use snap_actor_to_landscape when the ground is definitely the landscape and you want no trace at all, execute_batch for many actors.
//   @param actor_name - Name of the actor to snap (editor world).
//   @param trace_distance - Optional downward trace distance in cm; defaults to 10000
//   @param align_to_surface - When true, rotate actor to match the surface normal (trace hits only; the heightmap fallback has no normal); defaults to false
//   @param fallback_heightmap - When true (default) and the downward line trace misses (headless traces routinely miss the landscape), fall back to sampling the landscape heightmap at the actor's XY (same sampler as snap_actor_to_landscape) so the actor still lands. Set false to require a real trace hit.
//   @returns { actor: actor JSON, hit: bool, landed: bool, sampled_from: 'trace'|'heightmap', hit_actor?: string, hit_distance?: number }

TSharedPtr<FJsonObject> HandleSnapActorToGround(const TSharedPtr<FJsonObject>& Params)
{
    FString TargetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), TargetName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    float ProbeDepth = 10000.0f; // 100 m below the pivot
    Params->TryGetNumberField(TEXT("trace_distance"), ProbeDepth);

    bool bTiltToSurface = false;
    Params->TryGetBoolField(TEXT("align_to_surface"), bTiltToSurface);

    // Headless traces routinely pass straight through the landscape; unless told otherwise,
    // a miss falls back to sampling the heightmap at the actor's XY.
    bool bAllowHeightmap = true;
    Params->TryGetBoolField(TEXT("fallback_heightmap"), bAllowHeightmap);

    UWorld* EditorWorld = nullptr;
    TSharedPtr<FJsonObject> Failure;
    AActor* Target = UEAgentEditorCommandsImpl::ResolveEditorWorldActor(TargetName, EditorWorld, Failure);
    if (Target == nullptr)
    {
        return Failure;
    }

    FScopedTransaction SnapTransaction(UEAgentEditorCommandsImpl::UndoTitle(TEXT("Snap Actor To Ground"), TargetName));
    // Modify() before the transform write -- see set_actor_transform.
    Target->Modify();

    const FVector Pivot = Target->GetActorLocation();
    FVector BoundsCentre;
    FVector BoundsHalfSize;
    Target->GetActorBounds(/*bOnlyCollidingComponents=*/false, BoundsCentre, BoundsHalfSize);

    FHitResult Hit;
    const bool bHit = UEAgentEditorCommandsImpl::TraceStraightDown(EditorWorld, Target, Pivot, ProbeDepth, Hit);
    const bool bSolidHit = bHit && Hit.bBlockingHit;

    // The world Z the bounds' bottom must rest on: the trace hit, or the heightmap.
    double GroundZ = 0.0;
    bool bLanded = false;
    FString SampledFrom = TEXT("trace");
    bool bHeightmapInBounds = false;
    if (bSolidHit)
    {
        GroundZ = Hit.ImpactPoint.Z;
        bLanded = true;
    }
    else if (bAllowHeightmap)
    {
        // Same sampler as snap_actor_to_landscape.
        double HeightmapZ = 0.0;
        ALandscape* SampledLandscape = nullptr;
        FString SampleReason;
        if (SampleFirstLandscapeHeightAtXY(EditorWorld, Pivot.X, Pivot.Y,
                                           HeightmapZ, bHeightmapInBounds, SampledLandscape, SampleReason))
        {
            GroundZ = HeightmapZ;
            bLanded = true;
            SampledFrom = TEXT("heightmap");
        }
    }

    if (!bLanded)
    {
        return mcp_common::MakeErrorReply(bAllowHeightmap
            ? TEXT("No surface found below actor within trace distance and no landscape to sample")
            : TEXT("No surface found below actor within trace distance"));
    }

    // Seat the BOTTOM of the bounds on the ground, wherever the pivot sits inside them:
    // the pivot is (Pivot.Z - (BoundsCentre.Z - BoundsHalfSize.Z)) above that bottom.
    // (Adding the half-height alone only works for a pivot at the bounds' centre.)
    // The probe is vertical and the heightmap is sampled at the pivot, so X/Y stay.
    FVector Seated = Pivot;
    Seated.Z = GroundZ + (Pivot.Z - (BoundsCentre.Z - BoundsHalfSize.Z));

    FTransform Snapped = Target->GetTransform();
    Snapped.SetLocation(Seated);

    // Optional tilt onto the surface normal -- trace hits only, a heightmap sample has none.
    if (bTiltToSurface && bSolidHit && Hit.ImpactNormal.SizeSquared() > 0.01f)
    {
        const FQuat Tilt = FQuat::FindBetweenNormals(FVector::UpVector, Hit.ImpactNormal.GetSafeNormal());
        Snapped.SetRotation(Tilt * Snapped.GetRotation());
    }
    Target->SetActorTransform(Snapped);

    if (GEditor != nullptr)
    {
        GEditor->NoteSelectionChange();
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetObjectField(TEXT("actor"), mcp_common::DescribeActorObject(Target));
    Reply->SetBoolField(TEXT("hit"), bHit);
    Reply->SetBoolField(TEXT("landed"), bLanded);
    Reply->SetStringField(TEXT("sampled_from"), SampledFrom);
    if (bHit)
    {
        const AActor* HitOwner = Hit.GetActor();
        Reply->SetStringField(TEXT("hit_actor"), HitOwner != nullptr ? HitOwner->GetName() : FString());
        Reply->SetNumberField(TEXT("hit_distance"), Hit.Distance);
    }
    else if (SampledFrom == TEXT("heightmap"))
    {
        Reply->SetBoolField(TEXT("in_bounds"), bHeightmapInBounds);
    }
    return Reply;
}

// MCP:align_actors - Snap two or more editor-world actors onto a shared location, rotation and/or scale taken from a reference (the first, the last, their average, or a named actor). Use it to straighten a hand-placed row or make a set of props share an orientation, in one undoable step; execute_batch of set_actor_transform calls, with transaction=true, is the tool when each actor needs a DIFFERENT transform. WARNING: aligning "location" collapses them onto the same point -- for a row, align only rotation/scale and set positions yourself.
//   @param actor_names - Array of at least 2 INTERNAL actor names (GetName) in the editor world; fewer is an error, and editor labels do not resolve.
//   @param align_type - "location" (default), "rotation", "scale" or "all" -- which transform components get copied from the reference.
//   @param reference - Where the target values come from: "first" (default), "last", "average" (the componentwise mean of all of them), or the internal name of one of the listed actors.
//   @returns { aligned_count: int, align_type: string, reference: string, aligned_actors: [actor JSON] }

TSharedPtr<FJsonObject> HandleAlignActors(const TSharedPtr<FJsonObject>& Params)
{
    const TArray<TSharedPtr<FJsonValue>>* MemberNames = nullptr;
    if (!Params->TryGetArrayField(TEXT("actor_names"), MemberNames) || !MemberNames || MemberNames->Num() < 2)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: actor_names (array with at least 2 actors)"));
    }

    // What to copy: "location" (default), "rotation", "scale" or "all".
    FString Mode = TEXT("location");
    Params->TryGetStringField(TEXT("align_type"), Mode);
    // Where it comes from: "first" (default), "last", "average", or one member's internal name.
    FString Anchor = TEXT("first");
    Params->TryGetStringField(TEXT("reference"), Anchor);

    UWorld* EditorWorld = UEAgentEditorCommandsImpl::ValidEditorWorld();
    if (EditorWorld == nullptr)
    {
        return UEAgentEditorCommandsImpl::NoWorldReply();
    }

    const TArray<AActor*> Group = UEAgentEditorCommandsImpl::ResolveNameList(
        *MemberNames, UEAgentEditorCommandsImpl::SnapshotActors(EditorWorld), nullptr);
    if (Group.Num() < 2)
    {
        return mcp_common::MakeErrorReply(TEXT("Need at least 2 valid actors to align"));
    }

    FScopedTransaction AlignTransaction(FText::FromString(TEXT("Align Actors")));

    UEAgentEditorCommandsImpl::FAlignTarget Target;
    if (Anchor == TEXT("average"))
    {
        Target = UEAgentEditorCommandsImpl::AlignTargetAveraged(Group);
    }
    else if (Anchor == TEXT("last"))
    {
        Target = UEAgentEditorCommandsImpl::AlignTargetFrom(Group.Last());
    }
    else
    {
        // "first", or a member named explicitly; a name that matches no member falls back
        // to the first one.
        const AActor* Named = (Anchor == TEXT("first"))
            ? nullptr
            : UEAgentEditorCommandsImpl::PickByObjectName(Group, Anchor);
        Target = UEAgentEditorCommandsImpl::AlignTargetFrom(Named != nullptr ? Named : Group[0]);
    }

    const bool bEverything = Mode == TEXT("all");
    const bool bTakeLocation = bEverything || Mode == TEXT("location");
    const bool bTakeRotation = bEverything || Mode == TEXT("rotation");
    const bool bTakeScale = bEverything || Mode == TEXT("scale");

    TArray<TSharedPtr<FJsonValue>> AlignedJson;
    for (AActor* Member : Group)
    {
        FTransform Pose = Member->GetTransform();
        if (bTakeLocation)
        {
            Pose.SetLocation(Target.Location);
        }
        if (bTakeRotation)
        {
            Pose.SetRotation(FQuat(Target.Rotation));
        }
        if (bTakeScale)
        {
            Pose.SetScale3D(Target.Scale);
        }
        // Modify() before the write -- see set_actor_transform.
        Member->Modify();
        Member->SetActorTransform(Pose);
        AlignedJson.Add(mcp_common::DescribeActorValue(Member));
    }

    if (GEditor != nullptr && AlignedJson.Num() > 0)
    {
        GEditor->NoteSelectionChange();
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetNumberField(TEXT("aligned_count"), AlignedJson.Num());
    Reply->SetStringField(TEXT("align_type"), Mode);
    Reply->SetStringField(TEXT("reference"), Anchor);
    Reply->SetArrayField(TEXT("aligned_actors"), AlignedJson);
    return Reply;
}

// Instance components on a placed actor

// MCP:add_component_to_actor - Attach a new component of any class to one actor already placed in the editor world, as an instance component in one undo step; the actor's Blueprint is not changed. Pick it for a single placed actor; add_component_to_blueprint changes the Blueprint so every instance gets it, and a typed add_*_component tool (add_spline_component, add_camera_component...) also sets that class's fields. Each call adds another component, so a retry makes a duplicate; with idempotent=true an existing component of that class is returned instead (already_existed true). The reply's object_path feeds set_properties and call_function; component_name, suffixed when the name is taken, feeds remove_component_from_actor, the inverse. An unknown actor or a class that is not an actor component is an error. It dirties the actor's package; save_level keeps it.
//   @param actor_name - Internal object name (not the label) of the placed actor in the editor world, e.g. 'StaticMeshActor_3'.
//   @param component_type - Component class by name or path, with or without the U prefix or the Component suffix, e.g. 'StaticMeshComponent', 'Box' or '/Script/Engine.PointLightComponent'.
//   @param component_name - Name for the new component, e.g. 'TriggerBox'; a taken name gets a unique variant, reported back in component_name.
//   @param location - Optional [x, y, z] relative location (for scene components)
//   @param rotation - Optional [pitch, yaw, roll] relative rotation (for scene components)
//   @param scale - Optional [x, y, z] relative scale (for scene components)
//   @param idempotent - Optional bool (default false). When true, if the actor already has a component of this class, return it instead of adding a duplicate (already_existed:true). Use for single-instance components.
//   @returns { actor_name: string, component_name: string, object_path: string, requested_component_name: string, component_type: string, already_existed: bool, success: bool }
//   Note: object_path is the full reflection path -- feed it to set_properties / call_function to configure the component (no dedicated add_* wrapper needed).
//   @hints not-idempotent

TSharedPtr<FJsonObject> HandleAddComponentToActor(const TSharedPtr<FJsonObject>& Params)
{
    FString HostName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), HostName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString TypeName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("component_type"), TypeName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString WantedName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("component_name"), WantedName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UWorld* EditorWorld = nullptr;
    TSharedPtr<FJsonObject> Failure;
    AActor* Host = UEAgentEditorCommandsImpl::ResolveEditorWorldActor(HostName, EditorWorld, Failure);
    if (Host == nullptr)
    {
        return Failure;
    }

    UClass* ComponentClass = UEAgentEditorCommandsImpl::ResolveComponentClass(TypeName);
    if (ComponentClass == nullptr || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown or invalid component type: %s. Make sure the class name is correct (e.g., 'StaticMeshComponent' or '/Script/Engine.StaticMeshComponent')."), *TypeName));
    }

    // Opt-in single-instance mode: hand back an existing component of that class instead of
    // adding a duplicate. Off by default, which keeps "always add" (several meshes, say).
    bool bIdempotent = false;
    Params->TryGetBoolField(TEXT("idempotent"), bIdempotent);
    UActorComponent* Component = bIdempotent ? Host->FindComponentByClass(ComponentClass) : nullptr;
    const bool bAlreadyThere = (Component != nullptr);

    if (!bAlreadyThere)
    {
        FScopedTransaction AddTransaction(UEAgentEditorCommandsImpl::UndoTitle(TEXT("Add Component"), WantedName));
        Host->Modify();

        UActorComponent* Added = Host->AddComponentByClass(
            ComponentClass, /*bManualAttachment=*/false, FTransform::Identity, /*bDeferredFinish=*/false);
        if (Added == nullptr)
        {
            return mcp_common::MakeErrorReply(TEXT("Failed to add component to actor"));
        }

        // The requested name, or a unique variant of it when it is taken. Nothing happens when the
        // component already carries the name.
        if (Added->GetFName() != FName(*WantedName))
        {
            mcp_common::RenameToFreeName(Added, WantedName);
        }

        if (USceneComponent* AsScene = Cast<USceneComponent>(Added))
        {
            UEAgentEditorCommandsImpl::ApplyRelativePlacement(Params, AsScene);
        }

        // Keep it as an INSTANCE component: a later set_property whose PostEditChange reruns
        // the construction scripts would otherwise throw the new component away.
        Added->CreationMethod = EComponentCreationMethod::Instance;
        Host->AddInstanceComponent(Added);
        Added->RegisterComponent();

        if (GEditor != nullptr)
        {
            GEditor->NoteSelectionChange();
        }
        Component = Added;
    }

    // One reply for both outcomes. component_name is the name the component ACTUALLY carries
    // (it can differ from the requested one after a collision), so remove_component_from_actor
    // can round-trip it; component_type is the class that was asked for.
    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("actor_name"), HostName);
    Reply->SetStringField(TEXT("component_name"), Component->GetName());
    Reply->SetStringField(TEXT("object_path"), Component->GetPathName());
    Reply->SetStringField(TEXT("requested_component_name"), WantedName);
    Reply->SetStringField(TEXT("component_type"), ComponentClass->GetName());
    Reply->SetBoolField(TEXT("already_existed"), bAlreadyThere);
    Reply->SetBoolField(TEXT("success"), true);
    return Reply;
}

// MCP:remove_component_from_actor - Detach and destroy one component from an actor PLACED in the level, by component name -- the inverse of add_component_to_actor and, like it, an instance-level edit that does not touch the actor's Blueprint (use remove_component_from_blueprint for the class). Discover the exact component_name with get_actor_components first. Editor world only; dirties the actor's package, so save_level to keep it.
//   @param actor_name - Name of the placed actor (editor world).
//   @param component_name - Exact component name as reported by get_actor_components.
//   @returns { actor_name: string, removed_component: string, component_type: string, success: bool }

TSharedPtr<FJsonObject> HandleRemoveComponentFromActor(const TSharedPtr<FJsonObject>& Params)
{
    FString HostName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), HostName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString PartName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("component_name"), PartName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UWorld* EditorWorld = nullptr;
    TSharedPtr<FJsonObject> Failure;
    AActor* Host = UEAgentEditorCommandsImpl::ResolveEditorWorldActor(HostName, EditorWorld, Failure);
    if (Host == nullptr)
    {
        return Failure;
    }

    UActorComponent* Victim = UEAgentEditorCommandsImpl::OwnedComponentNamed(Host, PartName);
    if (Victim == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Component not found: %s"), *PartName));
    }
    // The root carries the actor's transform; it is never removed from here.
    if (Victim == Host->GetRootComponent())
    {
        return mcp_common::MakeErrorReply(TEXT("Cannot remove root component. Set a different component as root first."));
    }

    FScopedTransaction RemoveTransaction(UEAgentEditorCommandsImpl::UndoTitle(TEXT("Remove Component"), PartName));
    Host->Modify();

    // Read the class while the component still exists; DestroyComponent unregisters and
    // removes it from the actor.
    const FString RemovedType = Victim->GetClass()->GetName();
    Victim->DestroyComponent();

    if (GEditor != nullptr)
    {
        GEditor->NoteSelectionChange();
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("actor_name"), HostName);
    Reply->SetStringField(TEXT("removed_component"), PartName);
    Reply->SetStringField(TEXT("component_type"), RemovedType);
    Reply->SetBoolField(TEXT("success"), true);
    return Reply;
}

// =====================================================================================
// Level operations: level blueprint authoring + sub-level current/move.
// Engine APIs used (ENGINE_API / UNREALED_API exported, real impls):
//   ULevel::GetLevelScriptBlueprint(bool bDontCreate)            "Engine/Level.h"
//   UEditorLevelUtils::MakeLevelCurrent(ULevel*, bool)  -> void  "EditorLevelUtils.h"
//   UEditorLevelUtils::MoveActorsToLevel(...)           -> int32 "EditorLevelUtils.h"
//   UWorld::GetStreamingLevels / ULevelStreaming::GetWorldAssetPackageName / GetLoadedLevel
// =====================================================================================

namespace
{
    /**
     * Resolve a sub-level of the editor world by name. Shared by set_current_level and
     * move_actors_to_level. "persistent" (case-insensitive) returns the persistent level;
     * any other name is matched against the short package name of each streaming level.
     *
     * @param World          Editor world (must be valid).
     * @param LevelName      "persistent" or a streaming sub-level short name.
     * @param OutError       On failure, a human-readable, honest error message.
     * @return The resolved ULevel*, or nullptr on failure (OutError is then set).
     */
    ULevel* ResolveSubLevelByName(UWorld* World, const FString& LevelName, FString& OutError)
    {
        if (!World)
        {
            OutError = TEXT("No valid world found");
            return nullptr;
        }

        if (LevelName.Equals(TEXT("persistent"), ESearchCase::IgnoreCase))
        {
            ULevel* Persistent = World->PersistentLevel;
            if (!Persistent)
            {
                OutError = TEXT("World has no persistent level");
                return nullptr;
            }
            return Persistent;
        }

        // Match against streaming sub-levels by short package name.
        for (ULevelStreaming* Streaming : World->GetStreamingLevels())
        {
            if (!Streaming)
            {
                continue;
            }
            // GetShortName: pass the TCHAR* overload explicitly (the FString binds to
            // FStringView otherwise; both are equivalent here).
            const FString ShortName = FPackageName::GetShortName(*Streaming->GetWorldAssetPackageName());
            if (ShortName.Equals(LevelName, ESearchCase::IgnoreCase))
            {
                ULevel* Loaded = Streaming->GetLoadedLevel();
                if (!Loaded)
                {
                    OutError = FString::Printf(
                        TEXT("Streaming level not loaded/visible: %s"), *LevelName);
                    return nullptr;
                }
                return Loaded;
            }
        }

        OutError = FString::Printf(TEXT("Sub-level not found: %s"), *LevelName);
        return nullptr;
    }
}

// MCP:get_level_blueprint - Get (or, with create=true, bring into being) the current level's Level Blueprint and return the blueprint_path that the graph tools need -- its event graph is always called "EventGraph", so add_graph_node / connect_pins can target it directly with graph_name="EventGraph". This is the entry point for level-specific scripting that does not belong in any actor. Read-only by default: with create=false and no blueprint present it answers exists:false + success:true rather than failing.
//   @param create - Optional bool (default false). True creates the Level Blueprint if the level has none -- a mutation that dirties the map, so save_level afterwards.
//   @returns { exists: true, created: bool, blueprint_path: string, blueprint_name: string, event_graph: "EventGraph", level_name: string, success: true }, or, when no blueprint exists and create=false: { exists: false, success: true, level_name: string, note: string }
//   @hints additive

TSharedPtr<FJsonObject> HandleGetLevelBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    bool bCreate = false;
    Params->TryGetBoolField(TEXT("create"), bCreate);

    UWorld* World = UEAgentEditorCommandsImpl::ValidEditorWorld();
    if (World == nullptr)
    {
        return UEAgentEditorCommandsImpl::NoWorldReply();
    }

    ULevel* Level = World->GetCurrentLevel();
    if (!Level)
    {
        return mcp_common::MakeErrorReply(TEXT("Current level is null"));
    }

    // Query first with bDontCreate=true to tell already_existed from created.
    UE_LOG(LogTemp, Verbose, TEXT("get_level_blueprint: querying level '%s' (create=%s)"),
           *Level->GetName(), bCreate ? TEXT("true") : TEXT("false"));

    ULevelScriptBlueprint* ExistingLSB = Level->GetLevelScriptBlueprint(/*bDontCreate=*/true);
    bool bCreated = false;
    ULevelScriptBlueprint* LSB = ExistingLSB;

    if (!LSB)
    {
        if (!bCreate)
        {
            // Honest, non-fatal result: nothing to return, but the query succeeded.
            UE_LOG(LogTemp, Verbose, TEXT("get_level_blueprint: no level blueprint; create=false"));
            TSharedPtr<FJsonObject> NoneObj = MakeShared<FJsonObject>();
            NoneObj->SetBoolField(TEXT("exists"), false);
            NoneObj->SetBoolField(TEXT("success"), true);
            NoneObj->SetStringField(TEXT("level_name"), Level->GetName());
            NoneObj->SetStringField(TEXT("note"),
                TEXT("No level blueprint on the current level; pass create=true to create one."));
            return NoneObj;
        }

        // Create (bDontCreate=false materializes the blueprint).
        UE_LOG(LogTemp, Verbose, TEXT("get_level_blueprint: creating level blueprint"));
        LSB = Level->GetLevelScriptBlueprint(/*bDontCreate=*/false);
        bCreated = (LSB != nullptr);
        if (!LSB)
        {
            return mcp_common::MakeErrorReply(
                TEXT("Failed to create the level blueprint for the current level"));
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("exists"), true);
    ResultObj->SetBoolField(TEXT("created"), bCreated);
    ResultObj->SetStringField(TEXT("blueprint_path"), LSB->GetPathName());
    ResultObj->SetStringField(TEXT("blueprint_name"), LSB->GetName());
    ResultObj->SetStringField(TEXT("event_graph"), TEXT("EventGraph"));
    ResultObj->SetStringField(TEXT("level_name"), Level->GetName());
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// MCP:set_current_level - Choose which sub-level of the editor world is CURRENT, i.e. where newly spawned or pasted actors will land. Set it before spawn_actor / paste_actors when a map is split into streaming sub-levels, otherwise everything piles into whichever level happens to be active. Errors honestly if the named streaming level is not loaded/visible; already_current:true when it was already the one. Use move_actors_to_level to relocate actors that ended up in the wrong one.
//   @param level_name - "persistent" for the persistent level, or the SHORT name of a loaded, visible streaming sub-level (not a /Game path).
//   @returns { current_level: string, changed: bool, already_current: bool, success: true } -- current_level is the level current after the call; a level that is not found, or not loaded and visible, is an error instead.

TSharedPtr<FJsonObject> HandleSetCurrentLevel(const TSharedPtr<FJsonObject>& Params)
{
    // Which sub-level ("persistent" or a short streaming-level name)
    FString LevelName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("level_name"), LevelName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (LevelName.IsEmpty())
    {
        return UEAgentEditorCommandsImpl::EmptyParamReply(TEXT("level_name"));
    }

    UWorld* World = UEAgentEditorCommandsImpl::ValidEditorWorld();
    if (World == nullptr)
    {
        return UEAgentEditorCommandsImpl::NoWorldReply();
    }

    // Resolve the sub-level (honest error if missing / not loaded)
    FString ResolveError;
    ULevel* TargetLevel = ResolveSubLevelByName(World, LevelName, ResolveError);
    if (!TargetLevel)
    {
        return mcp_common::MakeErrorReply(ResolveError);
    }

    // Apply (MakeLevelCurrent is void -> compute `changed` by comparing before/after)
    ULevel* Before = World->GetCurrentLevel();
    UE_LOG(LogTemp, Verbose, TEXT("set_current_level: '%s' (current before='%s')"),
           *LevelName, Before ? *Before->GetName() : TEXT("<null>"));

    UEditorLevelUtils::MakeLevelCurrent(TargetLevel, /*bEvenIfLocked=*/true);

    ULevel* After = World->GetCurrentLevel();
    const bool bChanged = (Before != After);
    const bool bAlreadyCurrent = (Before == TargetLevel);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("current_level"), After ? After->GetName() : TEXT(""));
    Reply->SetBoolField(TEXT("changed"), bChanged);
    Reply->SetBoolField(TEXT("already_current"), bAlreadyCurrent);
    Reply->SetBoolField(TEXT("success"), true);
    return Reply;
}

// MCP:move_actors_to_level - Relocate existing editor-world actors into another sub-level (persistent or a loaded streaming level) -- it rescues actors that got spawned into the wrong level, which set_current_level only prevents going forward. Idempotent: actors already there come back in already_in_level instead of being touched. WARNING: it is clipboard-backed, so the editor SELECTION is mutated as a side effect, and the destination must be loaded AND unlocked. Both levels' packages get dirty: save_level after.
//   @param actor_names - Array of actor names OR editor labels in the editor world (this one does accept labels, unlike delete_actor/delete_actors).
//   @param target_level - "persistent" or the short name of a loaded streaming sub-level
//   @param move_all_or_fail - When true, fail the whole move if any actor cannot be moved; defaults to false
//   @returns { requested: int, moved_count: int, already_in_level: [string], target_level: string, success: bool, note: string }

TSharedPtr<FJsonObject> HandleMoveActorsToLevel(const TSharedPtr<FJsonObject>& Params)
{
    const TArray<TSharedPtr<FJsonValue>>* ActorNamesArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("actor_names"), ActorNamesArray) || !ActorNamesArray || ActorNamesArray->Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: actor_names (array of actor names)"));
    }

    FString TargetLevelName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target_level"), TargetLevelName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TargetLevelName.IsEmpty())
    {
        return UEAgentEditorCommandsImpl::EmptyParamReply(TEXT("target_level"));
    }

    bool bMoveAllOrFail = false;
    Params->TryGetBoolField(TEXT("move_all_or_fail"), bMoveAllOrFail);

    UWorld* World = UEAgentEditorCommandsImpl::ValidEditorWorld();
    if (World == nullptr)
    {
        return UEAgentEditorCommandsImpl::NoWorldReply();
    }

    // Resolve the target level (honest error if missing / not loaded)
    FString ResolveError;
    ULevel* DestLevel = ResolveSubLevelByName(World, TargetLevelName, ResolveError);
    if (!DestLevel)
    {
        return mcp_common::MakeErrorReply(ResolveError);
    }

    // Resolve actors by name AND label. Those already in DestLevel are skipped (idempotent).
    const int32 Requested = ActorNamesArray->Num();
    TArray<AActor*> ActorsToMove;
    TArray<TSharedPtr<FJsonValue>> AlreadyInLevel;
    TArray<FString> NotFound;

    for (const TSharedPtr<FJsonValue>& JsonValue : *ActorNamesArray)
    {
        FString Wanted;
        if (!JsonValue->TryGetString(Wanted) || Wanted.IsEmpty())
        {
            continue;
        }

        AActor* Found = mcp_common::FindActorByName(World, Wanted);
        if (!Found)
        {
            NotFound.Add(Wanted);
            continue;
        }

        if (Found->GetLevel() == DestLevel)
        {
            // Idempotent: already where it should be.
            AlreadyInLevel.Add(MakeShared<FJsonValueString>(Wanted));
            continue;
        }

        ActorsToMove.AddUnique(Found);
    }

    // Honest errors: actor not found.
    if (NotFound.Num() > 0)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Actor(s) not found: %s"), *FString::Join(NotFound, TEXT(", "))));
    }

    // Move (clipboard-backed; mutates the editor selection as a side effect).
    int32 Moved = 0;
    if (ActorsToMove.Num() > 0)
    {
        UE_LOG(LogTemp, Verbose, TEXT("move_actors_to_level: moving %d actor(s) to '%s'"),
               ActorsToMove.Num(), *TargetLevelName);
        Moved = UEditorLevelUtils::MoveActorsToLevel(
            ActorsToMove, DestLevel,
            /*bWarnAboutReferences=*/false, /*bWarnAboutRenaming=*/false,
            /*bMoveAllOrFail=*/bMoveAllOrFail);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetNumberField(TEXT("requested"), Requested);
    ResultObj->SetNumberField(TEXT("moved_count"), Moved);
    ResultObj->SetArrayField(TEXT("already_in_level"), AlreadyInLevel);
    ResultObj->SetStringField(TEXT("target_level"), TargetLevelName);
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("note"),
        TEXT("clipboard-backed move; the editor selection is mutated as a side effect; destination must be loaded and unlocked."));
    return ResultObj;
}

// ============================================================================
// Transaction commands
// ============================================================================
// All commands operate on GEditor->Trans (UTransactor*). Begin/End/Cancel are
// the non-RAII engine entry points. The open index is kept in the file-static
// ActiveTransactionIndex so End/Cancel match the correct transaction across
// separate MCP calls.

namespace
{
    // Resolve GEditor->Trans (UTransactor*) once. Returns nullptr on headless
    // failure -- handlers translate that into an honest error.
    UTransactor* GetEditorTransactor()
    {
        if (!GEditor) { return nullptr; }
        return GEditor->Trans;
    }
}

namespace UEAgentEditorCommandsImpl
{
    // The engine's GetUndoCount() is how many transactions have been UNDONE -- the redo side. What undo
    // can still revert is the rest of the queue.
    int32 UndoableCount(UTransactor* Trans)
    {
        return Trans->GetQueueLength() - Trans->GetUndoCount();
    }

    // The title of the entry undo (or redo) would apply next. CanUndo/CanRedo fill their text only with
    // the REASON they cannot, so the title comes from the transaction's context.
    FString NextLabel(UTransactor* Trans, bool bUndo)
    {
        if (bUndo)
        {
            return Trans->CanUndo() ? Trans->GetUndoContext(/*bCheckWhetherUndoPossible=*/false).Title.ToString() : FString();
        }
        return Trans->CanRedo() ? Trans->GetRedoContext().Title.ToString() : FString();
    }

    // The reply of undo (redo, when bUndo is false): steps the stack back (forward) by the optional `count`
    // transactions, default 1, stopping when the stack runs out (or an undo barrier stops it), and reports what
    // applied and what is next. A count below 1 is refused before anything is undone.
    TSharedPtr<FJsonObject> StepUndoStack(const TSharedPtr<FJsonObject>& Params, UTransactor* Trans, bool bUndo)
    {
        int32 Count = 1;
        Params->TryGetNumberField(TEXT("count"), Count);
        if (Count < 1)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("%s: count must be at least 1 (got %d); nothing was %s"),
                bUndo ? TEXT("undo") : TEXT("redo"), Count, bUndo ? TEXT("undone") : TEXT("redone")));
        }

        int32 Applied = 0;
        for (int32 Step = 0; Step < Count; ++Step)
        {
            const bool bApplied = bUndo ? Trans->Undo(/*bCanRedo=*/true) : Trans->Redo();
            if (!bApplied)
            {
                break;
            }
            ++Applied;
        }
        const bool bCanStepAgain = bUndo ? Trans->CanUndo() : Trans->CanRedo();

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("applied"), Applied > 0);
        Result->SetNumberField(TEXT("requested"), Count);
        Result->SetNumberField(bUndo ? TEXT("actually_undone") : TEXT("actually_redone"), Applied);
        Result->SetNumberField(TEXT("undo_count"), UndoableCount(Trans));
        Result->SetBoolField(bUndo ? TEXT("can_undo") : TEXT("can_redo"), bCanStepAgain);
        Result->SetStringField(bUndo ? TEXT("next_undo") : TEXT("next_redo"), NextLabel(Trans, bUndo));
        return Result;
    }
}

// MCP:begin_transaction - Open one undoable transaction so a whole batch of edits collapses into a SINGLE undo entry: every Modify() between this and end_transaction joins it, and cancel_transaction rolls the lot back without leaving anything on the stack. Use it to make a multi-step edit atomically reversible; the returned transaction_index is what end_transaction / cancel_transaction need. already_open:true means one was already running -- transactions do not nest here.
//   @param description - Label shown in the editor's undo history (e.g. 'MCP: retag turrets'). Make it recognisable; it is what get_transaction_history and next_undo report.
//   @param primary_object_path - Optional object path recorded as the transaction's PrimaryObject, so the undo entry is attributable to one asset/actor.
//   @returns { description: string, transaction_index: int, already_open: bool, undo_count: int }
TSharedPtr<FJsonObject> HandleBeginTransaction(const TSharedPtr<FJsonObject>& Params)
{
    UTransactor* Trans = GetEditorTransactor();
    if (!Trans)
    {
        return mcp_common::MakeErrorReply(TEXT("GEditor->Trans unavailable (run in editor)"));
    }

    FString Description;
    Params->TryGetStringField(TEXT("description"), Description);
    if (Description.IsEmpty()) { Description = TEXT("MCP transaction"); }

    FString PrimaryObjectPath;
    UObject* PrimaryObject = nullptr;
    if (Params->TryGetStringField(TEXT("primary_object_path"), PrimaryObjectPath) && !PrimaryObjectPath.IsEmpty())
    {
        PrimaryObject = LoadObject<UObject>(nullptr, *PrimaryObjectPath);
    }

    if (ActiveTransactionIndex != INDEX_NONE)
    {
        // Idempotent: begin_transaction already has a transaction open. The
        // engine doesn't support nesting Begin() without End(); we surface the
        // already-open index so the caller can End() it before opening a new one.
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("description"), Description);
        ResultObj->SetNumberField(TEXT("transaction_index"), ActiveTransactionIndex);
        ResultObj->SetBoolField(TEXT("already_open"), true);
        ResultObj->SetNumberField(TEXT("undo_count"), UEAgentEditorCommandsImpl::UndoableCount(Trans));
        return ResultObj;
    }

    mcp_common::ForgetAssetsMadeInOpenTransaction();
    const int32 NewIndex = Trans->Begin(TEXT("MCP"), FText::FromString(Description));
    if (PrimaryObject)
    {
        Trans->SetPrimaryUndoObject(PrimaryObject);
    }

    ActiveTransactionIndex = NewIndex;

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("description"), Description);
    ResultObj->SetNumberField(TEXT("transaction_index"), NewIndex);
    ResultObj->SetBoolField(TEXT("already_open"), false);
    ResultObj->SetNumberField(TEXT("undo_count"), UEAgentEditorCommandsImpl::UndoableCount(Trans));
    ResultObj->SetStringField(TEXT("primary_object"), PrimaryObject ? PrimaryObject->GetPathName() : FString());
    return ResultObj;
}

// MCP:end_transaction - Close the transaction opened by begin_transaction and COMMIT it, so everything done inside becomes one entry on the editor's undo stack (undo_count grows by one). Pair it with every begin_transaction -- leaving one open makes later edits silently join it. Use cancel_transaction instead when you want the batch thrown away rather than kept. A transaction that holds objects of the running Play-in-Editor session (a widget made with create_widget) is closed without an undo entry, recorded false: the edits stay, and keeping it would close the editor when PIE stops. So is one that recorded an asset made inside it, because undoing the making of an asset leaves it broken in memory.
//   @returns { success: bool, transaction_index: int (the one that was closed), recorded: bool (false: no undo entry, see note), undo_count: int after the commit, note?: string }
TSharedPtr<FJsonObject> HandleEndTransaction(const TSharedPtr<FJsonObject>& Params)
{
    UTransactor* Trans = GetEditorTransactor();
    if (!Trans)
    {
        return mcp_common::MakeErrorReply(TEXT("GEditor->Trans unavailable (run in editor)"));
    }

    if (ActiveTransactionIndex == INDEX_NONE)
    {
        return mcp_common::MakeErrorReply(
            TEXT("end_transaction called with no open transaction (begin_transaction opens one)"));
    }

    const int32 ClosedIdx = ActiveTransactionIndex;
    ActiveTransactionIndex = INDEX_NONE;

    // GUndo is the open transaction; the editor's buffer makes only FTransactions.
    const bool bHoldsPieSession = GEditor && GEditor->PlayWorld && GUndo
        && mcp_common::TransactionHoldsPieSessionObjects(static_cast<const FTransaction*>(GUndo));
    const FString NewAsset = mcp_common::AssetMadeInOpenTransaction();
    mcp_common::ForgetAssetsMadeInOpenTransaction();
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    if (!Trans->IsActive())
    {
        // Nothing is open any more: a reset of the undo history purged the transaction while it was open.
        ResultObj->SetBoolField(TEXT("success"), true);
        ResultObj->SetBoolField(TEXT("recorded"), false);
        ResultObj->SetStringField(TEXT("note"), TEXT("The undo history was reset while this transaction was open (a "
            "map load, reset_transaction_buffer, or the end of a Play-in-Editor session it held objects of), so "
            "nothing was recorded; the edits stay."));
    }
    else if (bHoldsPieSession)
    {
        // UTransBuffer::Cancel removes the entry without applying it: what was done inside stays.
        Trans->Cancel(0);
        ResultObj->SetBoolField(TEXT("success"), true);
        ResultObj->SetBoolField(TEXT("recorded"), false);
        ResultObj->SetStringField(TEXT("note"), TEXT("The transaction held objects of the running Play-in-Editor "
            "session, so it was closed without an undo entry: the edits stay, but undo cannot revert them. Kept, it "
            "would have closed the editor when PIE stops."));
    }
    else if (!NewAsset.IsEmpty())
    {
        // Undoing the making of an asset leaves it broken in memory (mcp_common::TransactionRecordedPackage).
        Trans->Cancel(0);
        ResultObj->SetBoolField(TEXT("success"), true);
        ResultObj->SetBoolField(TEXT("recorded"), false);
        ResultObj->SetStringField(TEXT("note"), FString::Printf(TEXT("The transaction recorded %s, an asset made inside "
            "it, and undoing the making of an asset would leave it broken in memory; so it was closed without an undo "
            "entry: the edits stay, but undo cannot revert them. delete_asset removes the asset."), *NewAsset));
    }
    else
    {
        const int32 EndedIndex = Trans->End();
        ResultObj->SetBoolField(TEXT("success"), EndedIndex != INDEX_NONE);
        ResultObj->SetBoolField(TEXT("recorded"), EndedIndex != INDEX_NONE);
    }
    ResultObj->SetNumberField(TEXT("transaction_index"), ClosedIdx);
    ResultObj->SetNumberField(TEXT("undo_count"), UEAgentEditorCommandsImpl::UndoableCount(Trans));
    return ResultObj;
}

// MCP:cancel_transaction - Abort the transaction opened by begin_transaction: the edits made inside it are rolled back (rolled_back true) and NO entry is written to the undo stack (undo_count is unchanged). The escape hatch when a multi-step edit goes wrong halfway -- cleaner than committing with end_transaction and then calling undo, which leaves history behind. Only what the transaction recorded rolls back: a change the editor does not record for undo (one to a Play-in-Editor object, for instance) stays, and rolled_back is false when nothing was recorded. A transaction that recorded an asset made inside it is not rolled back at all (rolled_back false, with a note): rolling back the making of an asset leaves it broken in memory.
//   @returns { success: bool, transaction_index: int (the one that was cancelled), rolled_back: bool (whether recorded edits were reverted), undo_count: int (unchanged -- nothing was recorded), note?: string }
TSharedPtr<FJsonObject> HandleCancelTransaction(const TSharedPtr<FJsonObject>& Params)
{
    UTransactor* Trans = GetEditorTransactor();
    if (!Trans)
    {
        return mcp_common::MakeErrorReply(TEXT("GEditor->Trans unavailable (run in editor)"));
    }

    if (ActiveTransactionIndex == INDEX_NONE)
    {
        return mcp_common::MakeErrorReply(
            TEXT("cancel_transaction called with no open transaction (begin_transaction opens one)"));
    }

    const int32 ClosedIdx = ActiveTransactionIndex;
    ActiveTransactionIndex = INDEX_NONE;

    // UTransBuffer::Cancel only removes the entry: the edits made inside stayed, though this tool said they were
    // rolled back. So the transaction is closed and then undone without a redo entry, which reverts what it recorded
    // and leaves the stack as it was. Begin put it last in the buffer; End drops it when it recorded nothing.
    const int32 LastIndex = Trans->GetQueueLength() - 1;
    const FTransaction* Open = LastIndex >= 0 ? Trans->GetTransaction(LastIndex) : nullptr;
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    bool bRolledBack = false;
    // Rolling back the making of an asset leaves it broken in memory (mcp_common::TransactionRecordedPackage), so a
    // transaction that recorded one is dropped as it is.
    const FString NewAsset = mcp_common::AssetMadeInOpenTransaction();
    mcp_common::ForgetAssetsMadeInOpenTransaction();
    if (!NewAsset.IsEmpty())
    {
        Trans->Cancel(0);
        ResultObj->SetStringField(TEXT("note"), FString::Printf(TEXT("The transaction recorded %s, an asset made inside "
            "it, and rolling back the making of an asset would leave it broken in memory; so nothing was rolled back: "
            "the edits stay. delete_asset removes the asset."), *NewAsset));
    }
    else
    {
        Trans->End();
        if (Trans->IsActive())
        {
            // A transaction opened inside this one was never closed: the lot is dropped without applying anything.
            Trans->Cancel(0);
            ResultObj->SetStringField(TEXT("note"), TEXT("Another transaction was still open inside this one, so the "
                "edits could not be rolled back; the transactions were dropped and the edits stay."));
        }
        else if (Open && Trans->GetQueueLength() == LastIndex + 1 && Trans->GetTransaction(LastIndex) == Open
            && !Open->HasExpired())
        {
            bRolledBack = Trans->Undo(/*bCanRedo=*/false);
        }
        else
        {
            ResultObj->SetStringField(TEXT("note"), TEXT("The transaction recorded no undoable change, so there was "
                "nothing to roll back."));
        }
    }

    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetNumberField(TEXT("transaction_index"), ClosedIdx);
    ResultObj->SetBoolField(TEXT("rolled_back"), bRolledBack);
    ResultObj->SetNumberField(TEXT("undo_count"), UEAgentEditorCommandsImpl::UndoableCount(Trans));
    return ResultObj;
}

// MCP:undo - Step the editor's undo stack back by one transaction, or by `count` of them, as Ctrl+Z does, and report what is now on top.
//   @para Use it to revert an edit made in the editor, by a tool or by a person: the stack is shared, so read get_undo_state first to see the label you are about to revert. One tool call that changed something is usually one transaction; begin_transaction and end_transaction make several edits one. Undo reverts objects in memory, not files: an edit already saved stays on disk until the next save.
//   @para Example: {"count": 3} undoes the last three transactions. It stops early at the bottom of the stack or at an undo barrier (set_undo_barrier), and actually_undone then falls short of requested. `success` is true even with nothing to undo: read `applied`, and re-read what you changed to confirm it.
//   @core
//   @param count - Optional. How many transactions to undo, at least 1 (default 1); stops early when the stack runs out or reaches an undo barrier. Below 1 is refused.
//   @returns { applied: bool, requested: int, actually_undone: int, undo_count: int, can_undo: bool, next_undo: string }
TSharedPtr<FJsonObject> HandleUndo(const TSharedPtr<FJsonObject>& Params)
{
    UTransactor* Trans = GetEditorTransactor();
    if (!Trans) { return mcp_common::MakeErrorReply(TEXT("GEditor->Trans unavailable")); }

    return UEAgentEditorCommandsImpl::StepUndoStack(Params, Trans, /*bUndo=*/true);
}

// MCP:redo - Re-apply the transaction that undo just reverted, or `count` of them, and report what remains, including next_redo. WARNING: `success` is ALWAYS true even with an empty redo stack -- read `applied` instead, and with a count compare actually_redone against requested. Note the redo stack is discarded as soon as any NEW edit is made after an undo, so a redo that answers applied:false usually means something was changed in between, not that the command failed.
//   @param count - Optional. How many transactions to redo, at least 1 (default 1); stops early when the redo stack runs out. Below 1 is refused.
//   @returns { applied: bool (THE oracle), requested: int, actually_redone: int (compare against requested), undo_count: int, can_redo: bool, next_redo: string (label of the next redoable entry) }
TSharedPtr<FJsonObject> HandleRedo(const TSharedPtr<FJsonObject>& Params)
{
    UTransactor* Trans = GetEditorTransactor();
    if (!Trans) { return mcp_common::MakeErrorReply(TEXT("GEditor->Trans unavailable")); }

    return UEAgentEditorCommandsImpl::StepUndoStack(Params, Trans, /*bUndo=*/false);
}

// MCP:get_undo_state - One call that answers everything you need before touching undo/redo: both counts, both next-entry LABELS, why undo or redo is impossible right now, and whether a transaction is still OPEN (transaction_open -- the thing that silently swallows later edits into someone else's batch). Read it BEFORE undo so you know what you are about to revert.
//   @returns { can_undo: bool, can_redo: bool, undo_reason?: string (why not, when can_undo is false), redo_reason?: string (why not, when can_redo is false), undo_count: int (entries undo can still revert), redo_count: int (undone entries redo can restore), next_undo: string, next_redo: string, transaction_open: bool, transaction_index: int }
TSharedPtr<FJsonObject> HandleGetUndoState(const TSharedPtr<FJsonObject>& Params)
{
    UTransactor* Trans = GetEditorTransactor();
    if (!Trans) { return mcp_common::MakeErrorReply(TEXT("GEditor->Trans unavailable")); }

    FText UndoReason;
    FText RedoReason;
    const bool bCanUndo = Trans->CanUndo(&UndoReason);
    const bool bCanRedo = Trans->CanRedo(&RedoReason);
    const int32 UndoCount = UEAgentEditorCommandsImpl::UndoableCount(Trans);
    const int32 RedoCount = Trans->GetUndoCount();  // the engine counts the undone entries

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("can_undo"), bCanUndo);
    ResultObj->SetBoolField(TEXT("can_redo"), bCanRedo);
    if (!bCanUndo && !UndoReason.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("undo_reason"), UndoReason.ToString());
    }
    if (!bCanRedo && !RedoReason.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("redo_reason"), RedoReason.ToString());
    }
    ResultObj->SetNumberField(TEXT("undo_count"), UndoCount);
    ResultObj->SetNumberField(TEXT("redo_count"), RedoCount);
    ResultObj->SetStringField(TEXT("next_undo"), UEAgentEditorCommandsImpl::NextLabel(Trans, /*bUndo=*/true));
    ResultObj->SetStringField(TEXT("next_redo"), UEAgentEditorCommandsImpl::NextLabel(Trans, /*bUndo=*/false));
    ResultObj->SetBoolField(TEXT("transaction_open"), ActiveTransactionIndex != INDEX_NONE);
    ResultObj->SetNumberField(TEXT("transaction_index"), ActiveTransactionIndex);
    return ResultObj;
}

// MCP:get_transaction_history - Read the editor's undo buffer as a list, most-recent-first: each entry's title, context, primary object and whether it has already been undone. Use it to answer "what changed the editor recently, and was it me" -- including edits made by the human in the UI -- before you undo anything or blame a tool for a state you did not expect. Pure read; get_undo_state gives the counts-only summary.
//   @param max_entries - Max entries to return, newest first (default 50, clamped to 1..500).
//   @returns { count: int, entries: [{ queue_index: int, title: string, context: string, is_transient: bool, primary_object: string, is_undone: bool }] }
TSharedPtr<FJsonObject> HandleGetTransactionHistory(const TSharedPtr<FJsonObject>& Params)
{
    UTransBuffer* TransBuffer = Cast<UTransBuffer>(GetEditorTransactor());
    if (!TransBuffer) { return mcp_common::MakeErrorReply(TEXT("Editor trans buffer not available")); }

    int32 MaxEntries = 50;
    Params->TryGetNumberField(TEXT("max_entries"), MaxEntries);
    if (MaxEntries < 1)   { MaxEntries = 1; }
    if (MaxEntries > 500) { MaxEntries = 500; }

    const int32 TotalLen = TransBuffer->GetQueueLength();
    const int32 StartIdx = FMath::Max(0, TotalLen - MaxEntries);

    TArray<TSharedPtr<FJsonValue>> Entries;
    for (int32 Idx = TotalLen - 1; Idx >= StartIdx; --Idx)
    {
        const FTransaction* Tx = TransBuffer->GetTransaction(Idx);
        if (!Tx) { continue; }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("queue_index"), Idx);
        Entry->SetStringField(TEXT("title"), Tx->GetTitle().ToString());
        Entry->SetStringField(TEXT("context"), Tx->GetContext().Context);
        Entry->SetBoolField(TEXT("is_transient"), Tx->IsTransient());
        Entry->SetStringField(TEXT("primary_object"),
            Tx->GetContext().PrimaryObject ? Tx->GetContext().PrimaryObject->GetPathName() : FString());
        // The engine counts the undone entries (GetUndoCount), and they are the last ones in the queue.
        Entry->SetBoolField(TEXT("is_undone"), Idx >= TotalLen - TransBuffer->GetUndoCount());
        Entries.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetNumberField(TEXT("count"), Entries.Num());
    ResultObj->SetArrayField(TEXT("entries"), Entries);
    return ResultObj;
}

// MCP:reset_transaction_buffer - IRREVERSIBLE: wipes the editor's ENTIRE undo AND redo history (the human's entries too, not just yours) and drops any open transaction. It does not revert anything -- edits already made stay made, they just stop being undoable. Use only to reclaim memory after a huge batch, or when a corrupt buffer is causing trouble; to undo work use undo (with `count` for several steps). undo_count_before vs after is the evidence.
//   @param reason - Human-readable reason, logged by the engine so the wipe is traceable. Defaults to 'MCP reset_transaction_buffer'.
//   @returns { success: bool (hardcoded), undo_count_before: int, undo_count_after: int (0) }
TSharedPtr<FJsonObject> HandleResetTransactionBuffer(const TSharedPtr<FJsonObject>& Params)
{
    UTransactor* Trans = GetEditorTransactor();
    if (!Trans) { return mcp_common::MakeErrorReply(TEXT("GEditor->Trans unavailable")); }

    FString Reason;
    Params->TryGetStringField(TEXT("reason"), Reason);
    if (Reason.IsEmpty()) { Reason = TEXT("MCP reset_transaction_buffer"); }

    const int32 Before = UEAgentEditorCommandsImpl::UndoableCount(Trans);
    Trans->Reset(FText::FromString(Reason));
    ActiveTransactionIndex = INDEX_NONE; // any open tx is gone after Reset

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetNumberField(TEXT("undo_count_before"), Before);
    ResultObj->SetNumberField(TEXT("undo_count_after"), UEAgentEditorCommandsImpl::UndoableCount(Trans));
    return ResultObj;
}

// MCP:set_undo_barrier - Drop a floor in the undo stack at the current position so no later undo can cross it -- the way to protect everything done SO FAR (yours or the human's) before starting a risky batch that you may want to unwind. Idempotent: a barrier already sitting at the cursor is not pushed twice (already_existed:true). Lift it with remove_undo_barrier (all=true lifts every barrier).
//   @returns { success: bool (hardcoded), already_existed: bool (true = nothing new was pushed), undo_barrier: int (cursor it sits at), undo_count: int, queue_length: int }
TSharedPtr<FJsonObject> HandleSetUndoBarrier(const TSharedPtr<FJsonObject>& Params)
{
    UTransBuffer* TransBuffer = Cast<UTransBuffer>(GetEditorTransactor());
    if (!TransBuffer) { return mcp_common::MakeErrorReply(TEXT("Editor trans buffer not available")); }

    // Cursor = position undo would leave the buffer at (queue top minus undone count).
    const int32 Cursor = TransBuffer->GetQueueLength() - TransBuffer->GetUndoCount();
    const bool bAlreadyExisted = (TransBuffer->GetCurrentUndoBarrier() == Cursor);
    if (!bAlreadyExisted)
    {
        TransBuffer->SetUndoBarrier();
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    ResultObj->SetNumberField(TEXT("undo_barrier"), TransBuffer->GetCurrentUndoBarrier());
    ResultObj->SetNumberField(TEXT("undo_count"), UEAgentEditorCommandsImpl::UndoableCount(TransBuffer));
    ResultObj->SetNumberField(TEXT("queue_length"), TransBuffer->GetQueueLength());
    return ResultObj;
}

// MCP:remove_undo_barrier - Lift the most recent undo barrier so undo can step past it again -- the counterpart of set_undo_barrier, for when the protected batch is finished -- or, with all=true, every barrier at once (including ones another session or the human placed to protect their work). Nothing is undone; it only lifts the floor. WARNING: `success` is hardcoded true: `removed` is the oracle and comes back false as an honest no-op when there was no barrier.
//   @param all - Optional bool (default false). true clears ALL undo barriers instead of popping the most recent one.
//   @returns { success: bool (hardcoded), removed: bool (THE oracle -- false = there was nothing to pop), all: bool (echo), undo_barrier: int now in force, undo_count: int, queue_length: int }
TSharedPtr<FJsonObject> HandleRemoveUndoBarrier(const TSharedPtr<FJsonObject>& Params)
{
    bool bAll = false;
    Params->TryGetBoolField(TEXT("all"), bAll);

    UTransBuffer* TransBuffer = Cast<UTransBuffer>(GetEditorTransactor());
    if (!TransBuffer) { return mcp_common::MakeErrorReply(TEXT("Editor trans buffer not available")); }

    const int32 Before = TransBuffer->GetCurrentUndoBarrier();
    if (bAll)
    {
        TransBuffer->ClearUndoBarriers();
    }
    else
    {
        TransBuffer->RemoveUndoBarrier();
    }
    const bool bRemoved = (Before != INDEX_NONE);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetBoolField(TEXT("removed"), bRemoved);
    ResultObj->SetBoolField(TEXT("all"), bAll);
    ResultObj->SetNumberField(TEXT("undo_barrier"), TransBuffer->GetCurrentUndoBarrier());
    ResultObj->SetNumberField(TEXT("undo_count"), UEAgentEditorCommandsImpl::UndoableCount(TransBuffer));
    ResultObj->SetNumberField(TEXT("queue_length"), TransBuffer->GetQueueLength());
    return ResultObj;
}

// ========================================================================== //
// snap_actor_to_landscape -- height-sample based grounding (no line trace)
// ========================================================================== //
namespace
{
    // Sample the landscape surface world-Z at world (WorldX, WorldY) straight from the
    // heightmap (LandscapeHeightSampling::SampleHeightAt, the headless-safe path of
    // get_landscape_height_at). Editor line traces miss the landscape headless, so
    // this never traces. Picks the first ALandscape in the world. Returns false (with
    // a reason via OutReason) when there is no usable landscape.
    bool SampleFirstLandscapeHeightAtXY(UWorld* World, double WorldX, double WorldY,
                                        double& OutWorldZ, bool& OutInBounds,
                                        ALandscape*& OutLandscape, FString& OutReason)
    {
        OutLandscape = nullptr;
        OutInBounds = false;
        if (!World)
        {
            OutReason = TEXT("No valid world found");
            return false;
        }

        ALandscape* Landscape = nullptr;
        for (TActorIterator<ALandscape> It(World); It; ++It)
        {
            if (*It)
            {
                Landscape = *It;
                break;
            }
        }
        if (!Landscape)
        {
            OutReason = TEXT("No ALandscape found in the editor world");
            return false;
        }

        ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
        if (!Info)
        {
            OutReason = TEXT("Landscape has no ULandscapeInfo (not fully imported)");
            return false;
        }

        int32 ExtMinX = 0, ExtMinY = 0, ExtMaxX = 0, ExtMaxY = 0;
        if (!Info->GetLandscapeExtent(ExtMinX, ExtMinY, ExtMaxX, ExtMaxY))
        {
            OutReason = TEXT("Could not resolve landscape extent");
            return false;
        }

        LandscapeHeightSampling::FHeightSample Sample;
        if (!LandscapeHeightSampling::SampleHeightAt(Landscape, Info, ExtMinX, ExtMinY, ExtMaxX, ExtMaxY, WorldX, WorldY, Sample))
        {
            OutReason = TEXT("Landscape has degenerate XY scale");
            return false;
        }
        OutWorldZ = Sample.WorldZ;
        OutInBounds = Sample.bInBounds;
        OutLandscape = Landscape;
        return true;
    }

    // Lowest world-Z of the actor's mesh geometry, computed as the min Z of the 8
    // transformed corners of each mesh component's LOCAL-space bounds. This is the
    // tight oriented-box bottom, unlike GetActorBounds (an inflated AABB that also
    // folds in interaction shape components). Prefers UMeshComponent (static /
    // skeletal / instanced meshes) so bare shape volumes don't drag the bottom
    // down; falls back to all primitive components if the actor has no mesh
    // components. Returns false when no usable geometry is found.
    bool ComputeMeshBBoxBottomWorldZ(AActor* Actor, double& OutMinZ)
    {
        if (!Actor)
        {
            return false;
        }

        bool bFound = false;
        double MinZ = TNumericLimits<double>::Max();

        auto Accumulate = [&](UPrimitiveComponent* Prim)
        {
            if (!Prim)
            {
                return;
            }
            const FBox LocalBox = Prim->CalcBounds(FTransform::Identity).GetBox();
            // Skip empty/degenerate components (e.g. a mesh component with no mesh),
            // whose bounds collapse to the component origin and would falsely lower
            // the bottom.
            if (!LocalBox.IsValid || LocalBox.GetExtent().IsNearlyZero())
            {
                return;
            }
            FVector Corners[8];
            LocalBox.GetVertices(Corners);
            const FTransform& CompXf = Prim->GetComponentTransform();
            for (const FVector& Corner : Corners)
            {
                const double WorldZ = CompXf.TransformPosition(Corner).Z;
                MinZ = FMath::Min(MinZ, WorldZ);
                bFound = true;
            }
        };

        TArray<UMeshComponent*> MeshComps;
        Actor->GetComponents<UMeshComponent>(MeshComps);
        for (UMeshComponent* MeshComp : MeshComps)
        {
            Accumulate(MeshComp);
        }

        if (!bFound)
        {
            TArray<UPrimitiveComponent*> Prims;
            Actor->GetComponents<UPrimitiveComponent>(Prims);
            for (UPrimitiveComponent* Prim : Prims)
            {
                Accumulate(Prim);
            }
        }

        if (bFound)
        {
            OutMinZ = MinZ;
        }
        return bFound;
    }
}

// MCP:snap_actor_to_landscape - Ground an actor on the landscape by sampling the heightmap at its XY (no line trace; traces miss the landscape headless). Sets the actor's Z so either its mesh bbox bottom or its origin sits at the sampled height + z_offset.
//   @renamed-from place_actor_on_terrain
//   @param actor_name - Internal name of the actor to place (Actor->GetName())
//   @param align - Optional 'mesh_bbox_bottom' (default; drops the lowest mesh-geometry point onto the ground) or 'actor_origin' (puts the actor origin at the ground)
//   @param z_offset - Optional extra world-Z added above the sampled ground height; defaults to 0
//   @returns { actor: actor JSON, landed_z: number (sampled ground world-Z), previous_z: number, new_z: number, sampled_from: 'heightmap', in_bounds: bool, align: string, align_used: string, z_offset: number, landscape_actor_name: string }

TSharedPtr<FJsonObject> HandlePlaceActorOnTerrain(const TSharedPtr<FJsonObject>& Params)
{
    FString TargetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), TargetName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString Align = TEXT("mesh_bbox_bottom");
    Params->TryGetStringField(TEXT("align"), Align);
    Align = Align.ToLower();
    if (Align != TEXT("mesh_bbox_bottom") && Align != TEXT("actor_origin"))
    {
        return mcp_common::MakeErrorReply(
            TEXT("Parameter 'align' must be one of: 'mesh_bbox_bottom', 'actor_origin'"));
    }

    double ZOffset = 0.0;
    Params->TryGetNumberField(TEXT("z_offset"), ZOffset);

    // Internal name only, like snap_actor_to_ground.
    UWorld* EditorWorld = nullptr;
    TSharedPtr<FJsonObject> Failure;
    AActor* Target = UEAgentEditorCommandsImpl::ResolveEditorWorldActor(TargetName, EditorWorld, Failure);
    if (Target == nullptr)
    {
        return Failure;
    }

    // The landscape height under the actor's XY, read from the heightmap itself.
    const FVector Start = Target->GetActorLocation();
    double LandedZ = 0.0;
    bool bInBounds = false;
    ALandscape* Landscape = nullptr;
    FString Reason;
    if (!SampleFirstLandscapeHeightAtXY(EditorWorld, Start.X, Start.Y, LandedZ, bInBounds, Landscape, Reason))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("snap_actor_to_landscape: %s"), *Reason));
    }

    // Undoable; Modify() also dirties the OFPA package (see snap_actor_to_ground).
    FScopedTransaction PlaceTransaction(UEAgentEditorCommandsImpl::UndoTitle(TEXT("Place Actor On Terrain"), TargetName));
    Target->Modify();

    FString AlignUsed;
    const double NewZ = UEAgentEditorCommandsImpl::GroundedActorZ(Target, Start.Z, LandedZ + ZOffset, Align, AlignUsed);
    UEAgentEditorCommandsImpl::SetActorWorldZ(Target, Start, NewZ);

    if (GEditor != nullptr)
    {
        GEditor->NoteSelectionChange();
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetObjectField(TEXT("actor"), mcp_common::DescribeActorObject(Target));
    Reply->SetNumberField(TEXT("landed_z"), LandedZ);
    Reply->SetNumberField(TEXT("previous_z"), Start.Z);
    Reply->SetNumberField(TEXT("new_z"), NewZ);
    Reply->SetStringField(TEXT("sampled_from"), TEXT("heightmap"));
    Reply->SetBoolField(TEXT("in_bounds"), bInBounds);
    Reply->SetStringField(TEXT("align"), Align);
    Reply->SetStringField(TEXT("align_used"), AlignUsed);
    Reply->SetNumberField(TEXT("z_offset"), ZOffset);
    Reply->SetStringField(TEXT("landscape_actor_name"), Landscape ? Landscape->GetActorLabel() : FString());
    return Reply;
}

// MCP:place_actors_on_terrain - Ground many actors on the landscape in one call by sampling the heightmap at each actor's XY (no line trace; scatter without per-point python). Same grounding math as snap_actor_to_landscape, applied per actor.
//   @retired execute_batch - one snap_actor_to_landscape per actor, with transaction=true for one undo step
//   @param actors - Array of { name (Actor->GetName()) OR label (Actor->GetActorLabel()), z_offset? (per-actor override), align? ('mesh_bbox_bottom'|'actor_origin', per-actor override) }
//   @param align - Optional default align for entries without their own; 'mesh_bbox_bottom' (default; drops the lowest mesh point) or 'actor_origin'
//   @param z_offset - Optional default extra world-Z above the sampled ground for entries without their own; defaults to 0
//   @returns { placed: [ { name, label, previous_z, new_z, landed_z, in_bounds, align, align_used, z_offset } ], failed: [ { name?, label?, error } ], placed_count: int, failed_count: int, requested_count: int, landscape_actor_name: string, world_name: string }

TSharedPtr<FJsonObject> HandleBatchPlaceActorsOnTerrain(const TSharedPtr<FJsonObject>& Params)
{
    // The entries, then the batch-wide defaults
    const TArray<TSharedPtr<FJsonValue>>* ActorsArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("actors"), ActorsArray) || !ActorsArray || ActorsArray->Num() == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: actors (array of {name|label, z_offset?, align?})"));
    }

    // Batch-level defaults for entries that don't override them.
    FString DefaultAlign = TEXT("mesh_bbox_bottom");
    Params->TryGetStringField(TEXT("align"), DefaultAlign);
    DefaultAlign = DefaultAlign.ToLower();
    if (DefaultAlign != TEXT("mesh_bbox_bottom") && DefaultAlign != TEXT("actor_origin"))
    {
        return mcp_common::MakeErrorReply(
            TEXT("Parameter 'align' must be one of: 'mesh_bbox_bottom', 'actor_origin'"));
    }

    double DefaultZOffset = 0.0;
    Params->TryGetNumberField(TEXT("z_offset"), DefaultZOffset);

    // The editor world (placement writes the actors' OFPA transforms)
    UWorld* World = UEAgentEditorCommandsImpl::ValidEditorWorld();
    if (World == nullptr)
    {
        return UEAgentEditorCommandsImpl::NoWorldReply();
    }

    // One pass over the level: index by internal name and by (non-empty) label. On a
    //    collision the first actor keeps the key; internal names are unique anyway.
    TMap<FString, AActor*> ByName;
    TMap<FString, AActor*> ByLabel;
    for (AActor* Each : UEAgentEditorCommandsImpl::SnapshotActors(World))
    {
        if (Each == nullptr)
        {
            continue;
        }
        ByName.FindOrAdd(Each->GetName(), Each);
        const FString Label = Each->GetActorLabel();
        if (!Label.IsEmpty())
        {
            ByLabel.FindOrAdd(Label, Each);
        }
    }

    // One transaction for the whole batch (a single undo). Modify() per actor.
    FScopedTransaction Transaction(FText::FromString(TEXT("Batch Place Actors On Terrain")));

    TArray<TSharedPtr<FJsonValue>> PlacedArray;
    TArray<TSharedPtr<FJsonValue>> FailedArray;
    ALandscape* LastLandscape = nullptr;

    auto MakeFailure = [](const FString& Name, const FString& Label, const FString& Error)
    {
        TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
        if (!Name.IsEmpty())  { F->SetStringField(TEXT("name"), Name); }
        if (!Label.IsEmpty()) { F->SetStringField(TEXT("label"), Label); }
        F->SetStringField(TEXT("error"), Error);
        return MakeShared<FJsonValueObject>(F);
    };

    for (const TSharedPtr<FJsonValue>& Item : *ActorsArray)
    {
        const TSharedPtr<FJsonObject>* ItemObj = nullptr;
        if (!Item->TryGetObject(ItemObj) || !ItemObj || !ItemObj->IsValid())
        {
            FailedArray.Add(MakeFailure(FString(), FString(), TEXT("Entry is not an object")));
            continue;
        }

        FString ReqName;
        FString ReqLabel;
        (*ItemObj)->TryGetStringField(TEXT("name"), ReqName);
        (*ItemObj)->TryGetStringField(TEXT("label"), ReqLabel);
        if (ReqName.IsEmpty() && ReqLabel.IsEmpty())
        {
            FailedArray.Add(MakeFailure(ReqName, ReqLabel, TEXT("Entry needs 'name' or 'label'")));
            continue;
        }

        // Resolve the actor: 'name' (internal name) first, then 'label'.
        AActor* TargetActor = nullptr;
        if (!ReqName.IsEmpty())
        {
            if (AActor** Found = ByName.Find(ReqName)) { TargetActor = *Found; }
        }
        if (!TargetActor && !ReqLabel.IsEmpty())
        {
            if (AActor** Found = ByLabel.Find(ReqLabel)) { TargetActor = *Found; }
        }
        if (!TargetActor)
        {
            FailedArray.Add(MakeFailure(ReqName, ReqLabel, TEXT("Actor not found")));
            continue;
        }

        // Per-actor overrides of the batch defaults.
        FString Align = DefaultAlign;
        FString RawAlign;
        if ((*ItemObj)->TryGetStringField(TEXT("align"), RawAlign))
        {
            RawAlign = RawAlign.ToLower();
            if (RawAlign != TEXT("mesh_bbox_bottom") && RawAlign != TEXT("actor_origin"))
            {
                FailedArray.Add(MakeFailure(ReqName, ReqLabel,
                    TEXT("'align' must be 'mesh_bbox_bottom' or 'actor_origin'")));
                continue;
            }
            Align = RawAlign;
        }

        double ZOffset = DefaultZOffset;
        (*ItemObj)->TryGetNumberField(TEXT("z_offset"), ZOffset);

        // Sample the landscape height at the actor's XY.
        const FVector ActorLocation = TargetActor->GetActorLocation();
        const double PreviousZ = ActorLocation.Z;
        double LandedZ = 0.0;
        bool bInBounds = false;
        ALandscape* Landscape = nullptr;
        FString Reason;
        if (!SampleFirstLandscapeHeightAtXY(World, ActorLocation.X, ActorLocation.Y,
                                            LandedZ, bInBounds, Landscape, Reason))
        {
            FailedArray.Add(MakeFailure(ReqName, ReqLabel, Reason));
            continue;
        }
        if (Landscape)
        {
            LastLandscape = Landscape;
        }

        TargetActor->Modify();

        // The same grounding math as snap_actor_to_landscape.
        FString AlignUsed;
        const double NewActorZ = UEAgentEditorCommandsImpl::GroundedActorZ(
            TargetActor, PreviousZ, LandedZ + ZOffset, Align, AlignUsed);
        UEAgentEditorCommandsImpl::SetActorWorldZ(TargetActor, ActorLocation, NewActorZ);

        TSharedPtr<FJsonObject> Placed = MakeShared<FJsonObject>();
        Placed->SetStringField(TEXT("name"), TargetActor->GetName());
        Placed->SetStringField(TEXT("label"), TargetActor->GetActorLabel());
        Placed->SetNumberField(TEXT("previous_z"), PreviousZ);
        Placed->SetNumberField(TEXT("new_z"), NewActorZ);
        Placed->SetNumberField(TEXT("landed_z"), LandedZ);
        Placed->SetBoolField(TEXT("in_bounds"), bInBounds);
        Placed->SetStringField(TEXT("align"), Align);
        Placed->SetStringField(TEXT("align_used"), AlignUsed);
        Placed->SetNumberField(TEXT("z_offset"), ZOffset);
        PlacedArray.Add(MakeShared<FJsonValueObject>(Placed));
    }

    if (GEditor && PlacedArray.Num() > 0)
    {
        GEditor->NoteSelectionChange();
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetArrayField(TEXT("placed"), PlacedArray);
    Reply->SetArrayField(TEXT("failed"), FailedArray);
    Reply->SetNumberField(TEXT("placed_count"), PlacedArray.Num());
    Reply->SetNumberField(TEXT("failed_count"), FailedArray.Num());
    Reply->SetNumberField(TEXT("requested_count"), ActorsArray->Num());
    Reply->SetStringField(TEXT("landscape_actor_name"),
        LastLandscape ? LastLandscape->GetActorLabel() : FString());
    Reply->SetStringField(TEXT("world_name"), World->GetName());
    return Reply;
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_component_to_actor"), &HandleAddComponentToActor },
    { TEXT("align_actors"), &HandleAlignActors },
    { TEXT("delete_actors"), &HandleBatchDeleteActors },
    { TEXT("place_actors_on_terrain"), &HandleBatchPlaceActorsOnTerrain },
    { TEXT("set_actor_transforms"), &HandleBatchSetActorTransform },
    { TEXT("begin_transaction"), &HandleBeginTransaction },
    { TEXT("cancel_transaction"), &HandleCancelTransaction },
    { TEXT("copy_actors"), &HandleCopyActors },
    { TEXT("delete_actor"), &HandleDeleteActor },
    { TEXT("deselect_actors"), &HandleDeselectActors },
    { TEXT("duplicate_actor"), &HandleDuplicateActor },
    { TEXT("end_transaction"), &HandleEndTransaction },
    { TEXT("find_actors_by_name"), &HandleFindActorsByName },
    { TEXT("get_actors_in_level"), &HandleGetActorsInLevel },
    { TEXT("get_current_level"), &HandleGetCurrentLevel },
    { TEXT("get_level_blueprint"), &HandleGetLevelBlueprint },
    { TEXT("get_selected_actors"), &HandleGetSelectedActors },
    { TEXT("get_transaction_history"), &HandleGetTransactionHistory },
    { TEXT("get_undo_state"), &HandleGetUndoState },
    { TEXT("load_level"), &HandleLoadLevel },
    { TEXT("move_actors_to_level"), &HandleMoveActorsToLevel },
    { TEXT("paste_actors"), &HandlePasteActors },
    { TEXT("snap_actor_to_landscape"), &HandlePlaceActorOnTerrain },
    { TEXT("redo"), &HandleRedo },
    { TEXT("remove_component_from_actor"), &HandleRemoveComponentFromActor },
    { TEXT("remove_undo_barrier"), &HandleRemoveUndoBarrier },
    { TEXT("reset_transaction_buffer"), &HandleResetTransactionBuffer },
    { TEXT("save_level"), &HandleSaveLevel },
    { TEXT("select_actors"), &HandleSelectActors },
    { TEXT("set_actor_transform"), &HandleSetActorTransform },
    { TEXT("set_current_level"), &HandleSetCurrentLevel },
    { TEXT("set_undo_barrier"), &HandleSetUndoBarrier },
    { TEXT("snap_actor_to_ground"), &HandleSnapActorToGround },
    { TEXT("spawn_actor"), &HandleSpawnActor },
    { TEXT("undo"), &HandleUndo },
};
} // namespace UEAgentEditorCommands
