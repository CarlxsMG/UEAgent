// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Misc/EngineVersionComparison.h"
#include "FileHelpers.h" // FEditorFileUtils::GetDirty{World,Content}Packages — the unsaved-work advisory on start_play_in_editor
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#include "HAL/PlatformProcess.h"
#include "Async/TaskGraphInterfaces.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "UnrealClient.h"
#include "Slate/SceneViewport.h"
// capture_pie_screenshot include_ui: crop the whole-window screenshot down to the game
// viewport via UGameViewportClient::OnScreenshotCaptured() + Slate widget geometry.
// (Containers/Ticker.h for the safety FTSTicker is already included below.)
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/WidgetPath.h"
#include "Widgets/SWindow.h"
#include "Widgets/SViewport.h"
#include "Misc/App.h" // FApp::CanEverRender() gate for the opt-in HUD screenshot
#include "Editor.h"
#include "Misc/FileHelper.h"
#include "ImageUtils.h"
#include "Modules/ModuleManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
// AWorldSettings
#include "GameFramework/WorldSettings.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/SpectatorPawn.h"
#include "GameFramework/DamageType.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/HUD.h"
// Navigation System - Reference: UNavigationSystemV1 (Runtime/NavigationSystem)
#include "NavigationSystem.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavigationData.h"
#include "ScopedTransaction.h"
#include "Templates/Function.h" // TFunctionRef (radius visitor)
#include "Components/BrushComponent.h"
// spawn_navmesh_bounds_volume: build a REAL cube brush (UCubeBuilder + UActorFactory), then report
// its geometry via UModel/UPolys -- same proven path as spawn_box_volume / spawn_nav_modifier_volume.
#include "ActorFactories/ActorFactory.h"
#include "Builders/CubeBuilder.h"
#include "Model.h"
#include "Engine/Polys.h"
#include "Containers/Ticker.h" // inject_analog_input hold_seconds: FTSTicker sustain across PIE ticks
// preserve_scale_ratio uses UEditorPerProjectUserSettings
#include "Editor/EditorPerProjectUserSettings.h"
// Play-in-Editor (PIE) includes - Reference: ULevelEditorSubsystem (Editor/LevelEditor)
#include "Editor/LevelEditor/Public/LevelEditorSubsystem.h"
#include "EditorSubsystem.h"
// Real PlayInEditor session request (multi-client/net-mode) + input injection.
#include "LevelEditor.h"                       // FLevelEditorModule::GetFirstActiveViewport
#include "IAssetViewport.h"                     // IAssetViewport
#include "PlayInEditorDataTypes.h"              // FRequestPlaySessionParams / EPlaySessionWorldType
#include "Settings/LevelEditorPlaySettings.h"   // ULevelEditorPlaySettings / EPlayNetMode
#include "InputKeyEventArgs.h"                  // FInputKeyEventArgs::CreateSimulated
#include "InputCoreTypes.h"                     // FKey / EKeys
#include "Engine/EngineBaseTypes.h"             // EInputEvent
#include "GameFramework/InputSettings.h"        // UInputSettings::GetInputSettings/bFilterInputByPlatformUser
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyPortFlags.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Blueprint/BlueprintSupport.h"
#include "Engine/Selection.h"

namespace UEAgentLevelCommands
{

// MCP-DOMAIN: Level

// Maps live actors (in whichever world set_actor_lifetime last targeted -- editor or PIE/game, see
// mcp_common::GetActiveWorld) to the lifetime-timer handle scheduled on them, so set_actor_lifetime can
// re-schedule idempotently. Entries go stale harmlessly when their world (e.g. a PIE session) ends.
static TMap<TWeakObjectPtr<AActor>, FTimerHandle> ActorLifetimeTimers;

// Every actor of World whose class NAME equals ClassName (case-insensitive, whole name, subclasses
// not included), at most MaxResults of them. The live actors are walked directly, so nothing is
// loaded to answer it.
TArray<AActor*> CollectActorsOfClass(UWorld* World, const FString& ClassName, int32 MaxResults = 1000)
{
    TArray<AActor*> Matches;
    if (World == nullptr || ClassName.IsEmpty())
    {
        return Matches;
    }
    for (TActorIterator<AActor> It(World); It && Matches.Num() < MaxResults; ++It)
    {
        if (It->GetClass()->GetName() == ClassName)
        {
            Matches.Add(*It);
        }
    }
    return Matches;
}

// ---------------------------------------------------------------------------------------------
// Helpers for the level commands below.
// ---------------------------------------------------------------------------------------------
namespace UEAgentLevelCommandsImpl
{
    /**
     * The world selected by the request's `world` field, through the resolver every domain shares
     * (mcp_common::ResolveWorldSelector): two tools resolving "PIE" on their own can read two
     * different worlds -- raycast_scene on UEDPIE_0 while get_actor_properties_by_class reads
     * UEDPIE_1 -- two correct readings that look like a game bug.
     *
     * The level domain's default is 'editor' when nothing is asked for -- GetEditorWorld(), GEditor's
     * own edit context, asked for by name, not by its position in GetWorldContexts() -- and 'game'
     * is an alias of 'pie'. OutWorldLabel echoes the selector used for the response's `world` field;
     * the AUTHORITATIVE identity of the world read is stamped by mcp_common::AddWorldEvidence.
     *
     * When the world does not resolve -- or, with bRequireValidLowLevel, is not a valid low-level
     * object -- OutFailure receives the resolver's reason, or FallbackMessage when it gave none.
     */
    UWorld* ResolveWorldForQuery(const TSharedPtr<FJsonObject>& Params, FString& OutWorldLabel,
                                 TSharedPtr<FJsonObject>& OutFailure, const TCHAR* FallbackMessage,
                                 bool bRequireValidLowLevel = false)
    {
        FString Reason;
        UWorld* World = mcp_common::ResolveWorldSelector(Params, TEXT("editor"), OutWorldLabel, Reason);
        const bool bUsable = World != nullptr && (!bRequireValidLowLevel || World->IsValidLowLevel());
        if (!bUsable)
        {
            OutFailure = mcp_common::MakeErrorReply(Reason.IsEmpty() ? FString(FallbackMessage) : Reason);
            return nullptr;
        }
        return World;
    }

    /** The actor named or labelled ActorName in World, or nullptr with OutFailure set. */
    AActor* FindNamedActor(UWorld* World, const FString& ActorName, const FString& WorldLabel, TSharedPtr<FJsonObject>& OutFailure)
    {
        AActor* Found = mcp_common::FindActorByName(World, ActorName);
        if (Found == nullptr)
        {
            OutFailure = mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Actor not found: %s (world='%s')"), *ActorName, *WorldLabel));
        }
        return Found;
    }

    /**
     * The actor the required actor_name param names or labels, in the world the request selects;
     * nullptr with OutFailure set when the param is missing or the world or the actor is not found.
     */
    AActor* ResolveActorForQuery(const TSharedPtr<FJsonObject>& Params, FString& OutActorName, UWorld*& OutWorld,
                                 FString& OutWorldLabel, TSharedPtr<FJsonObject>& OutFailure)
    {
        if (!Params->TryGetStringField(TEXT("actor_name"), OutActorName))
        {
            OutFailure = mcp_common::MakeErrorReply(TEXT("Missing 'actor_name' parameter"));
            return nullptr;
        }
        OutWorld = ResolveWorldForQuery(Params, OutWorldLabel, OutFailure, TEXT("No world found"));
        if (OutWorld == nullptr)
        {
            return nullptr;
        }
        return FindNamedActor(OutWorld, OutActorName, OutWorldLabel, OutFailure);
    }

    /**
     * The EDITOR-world actor the required actor_name param names or labels; nullptr with OutFailure
     * set when there is no editor world, the param is missing or the actor is not found.
     */
    AActor* ResolveEditorActor(const TSharedPtr<FJsonObject>& Params, FString& OutActorName, TSharedPtr<FJsonObject>& OutFailure)
    {
        UWorld* EditorWorld = mcp_common::GetEditorWorld();
        if (EditorWorld == nullptr)
        {
            OutFailure = mcp_common::MakeErrorReply(TEXT("No editor world available"));
            return nullptr;
        }
        if (!Params->TryGetStringField(TEXT("actor_name"), OutActorName))
        {
            OutFailure = mcp_common::MissingParameterReply(TEXT("actor_name"));
            return nullptr;
        }
        AActor* Found = mcp_common::FindActorByName(EditorWorld, OutActorName);
        if (Found == nullptr)
        {
            OutFailure = mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *OutActorName));
        }
        return Found;
    }

    /** Three flat number fields: <Prefix>x, <Prefix>y and <Prefix>z. */
    void WriteFlatXYZ(const TSharedPtr<FJsonObject>& Target, const TCHAR* Prefix, const FVector& Value)
    {
        const FString Stem(Prefix);
        Target->SetNumberField(Stem + TEXT("x"), Value.X);
        Target->SetNumberField(Stem + TEXT("y"), Value.Y);
        Target->SetNumberField(Stem + TEXT("z"), Value.Z);
    }

    /** The class name, or "None" when there is no class. */
    FString ClassNameOrNone(const UClass* Class)
    {
        return Class != nullptr ? Class->GetName() : FString(TEXT("None"));
    }

    /**
     * Calls Visit(Actor, Location, Distance) for every actor of World whose ORIGIN lies within
     * Radius of Center. The distance is taken in single precision, like the radius it is held to.
     */
    void ForEachActorWithinRadius(UWorld* World, const FVector& Center, float Radius,
                                  TFunctionRef<void(AActor*, const FVector&, float)> Visit)
    {
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Candidate = *It;
            if (Candidate == nullptr)
            {
                continue;
            }
            const FVector Where = Candidate->GetActorLocation();
            const float Distance = static_cast<float>(FVector::Dist(Center, Where));
            if (Distance <= Radius)
            {
                Visit(Candidate, Where, Distance);
            }
        }
    }

    /** One `components` row: identity, activity, editability and mesh / light details. */
    TSharedPtr<FJsonObject> DescribeComponent(UActorComponent* Component)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Component->GetName());
        Row->SetStringField(TEXT("class"), Component->GetClass()->GetName());
        Row->SetBoolField(TEXT("is_active"), Component->IsActive());
        Row->SetBoolField(TEXT("is_editable"), Component->HasAnyFlags(RF_Transactional));

        if (const UStaticMeshComponent* StaticPart = Cast<UStaticMeshComponent>(Component))
        {
            Row->SetStringField(TEXT("type"), TEXT("StaticMesh"));
            if (const UStaticMesh* Mesh = StaticPart->GetStaticMesh())
            {
                Row->SetStringField(TEXT("mesh_name"), Mesh->GetName());
            }
        }
        else if (const USkeletalMeshComponent* SkinnedPart = Cast<USkeletalMeshComponent>(Component))
        {
            Row->SetStringField(TEXT("type"), TEXT("SkeletalMesh"));
            if (const USkeletalMesh* Mesh = SkinnedPart->GetSkeletalMeshAsset())
            {
                Row->SetStringField(TEXT("mesh_name"), Mesh->GetName());
            }
        }
        else if (const ULightComponent* LightPart = Cast<ULightComponent>(Component))
        {
            Row->SetStringField(TEXT("type"), TEXT("Light"));
            Row->SetNumberField(TEXT("intensity"), LightPart->Intensity);
            Row->SetStringField(TEXT("light_color"), LightPart->LightColor.ToString());
        }
        else
        {
            Row->SetStringField(TEXT("type"), TEXT("Other"));
        }
        return Row;
    }

    /**
     * A class by name or path: an in-memory lookup first; then paths go through the typed class
     * loader and bare names are tried as an object path as they are.
     */
    template <typename TBase>
    UClass* FindOrLoadClass(const FString& NameOrPath)
    {
        if (UClass* InMemory = FindObject<UClass>(nullptr, *NameOrPath))
        {
            return InMemory;
        }
        if (NameOrPath.Contains(TEXT("/")))
        {
            return LoadClass<TBase>(nullptr, *NameOrPath);
        }
        return LoadObject<UClass>(nullptr, *NameOrPath);
    }

    /** "Standalone" / "ListenServer" / "Client" (any case) -> EPlayNetMode. */
    bool ParsePlayNetMode(const FString& Text, EPlayNetMode& OutMode)
    {
        struct FModeName
        {
            const TCHAR* Name;
            EPlayNetMode Mode;
        };
        static const FModeName KnownModes[] = {
            { TEXT("Standalone"), PIE_Standalone },
            { TEXT("ListenServer"), PIE_ListenServer },
            { TEXT("Client"), PIE_Client },
        };
        for (const FModeName& Candidate : KnownModes)
        {
            if (Text.Equals(Candidate.Name, ESearchCase::IgnoreCase))
            {
                OutMode = Candidate.Mode;
                return true;
            }
        }
        return false;
    }

    /** The PIE client a command targets: the pie_client_index param, 0 when absent. */
    int32 ReadPieClientIndex(const TSharedPtr<FJsonObject>& Params)
    {
        double Requested = 0.0;
        if (Params->TryGetNumberField(TEXT("pie_client_index"), Requested))
        {
            return static_cast<int32>(Requested);
        }
        return 0;
    }

    /** A named gravity setting of set_world_gravity's `preset`. */
    struct FGravityPreset
    {
        const TCHAR* Name;
        double GravityZ;
        bool bEnabled;
    };

    /** The preset called Name; FString equality ignores case, so any capitalisation matches. */
    const FGravityPreset* FindGravityPreset(const FString& Name)
    {
        static const FGravityPreset Presets[] = {
            { TEXT("Enabled"), -980.0, true },
            { TEXT("Disabled"), 0.0, false },
            { TEXT("Earth"), -980.0, true },
            { TEXT("Moon"), -163.0, true },
            { TEXT("Mars"), -371.0, true },
            { TEXT("Zero"), 0.0, true },
            { TEXT("Low"), -490.0, true },
            { TEXT("High"), -1960.0, true },
        };
        for (const FGravityPreset& Preset : Presets)
        {
            if (Name == Preset.Name)
            {
                return &Preset;
            }
        }
        return nullptr;
    }

    /**
     * Rough 0..~2.8 score of how much a gravity setting matters for this world: the gravity
     * relative to Earth's, plus weights for the simulating and the movable primitives. Zero when
     * gravity is disabled.
     */
    float GravityImpactScore(bool bGravityOn, double GravityZ, int32 SimulatingCount, int32 MovableCount)
    {
        float Score = 0.0f;
        if (bGravityOn)
        {
            Score += FMath::Abs(GravityZ) / 980.0f;
            Score += FMath::Clamp(SimulatingCount / 25.0f, 0.0f, 1.0f) * 0.5f;
            Score += FMath::Clamp(MovableCount / 20.0f, 0.0f, 1.0f) * 0.3f;
        }
        return Score;
    }

    const TCHAR* GravityImpactRating(float Score)
    {
        if (Score >= 0.8f)
        {
            return TEXT("High");
        }
        if (Score >= 0.5f)
        {
            return TEXT("Medium");
        }
        return Score >= 0.3f ? TEXT("Low") : TEXT("Minimal");
    }

    /** The advisory notes set_world_gravity attaches, in their fixed order. */
    TArray<TSharedPtr<FJsonValue>> GravityAdvice(bool bGravityOn, double GravityZ, int32 SimulatingCount, int32 MovableCount)
    {
        struct FNote
        {
            bool bApplies;
            const TCHAR* Text;
        };
        const double Magnitude = FMath::Abs(GravityZ);
        const FNote Notes[] = {
            { bGravityOn && Magnitude > 1500.0f, TEXT("High gravity detected - consider reducing for performance") },
            { bGravityOn && SimulatingCount > 30, TEXT("High simulating physics count with gravity - consider optimization") },
            { bGravityOn && MovableCount > 20, TEXT("High dynamic physics count with gravity - consider static optimization") },
            { !bGravityOn && SimulatingCount > 0, TEXT("Gravity disabled but physics simulation active - consider enabling gravity") },
            { bGravityOn && Magnitude < 100.0f, TEXT("Very low gravity - consider increasing for realistic physics") },
        };
        TArray<TSharedPtr<FJsonValue>> Advice;
        for (const FNote& Note : Notes)
        {
            if (Note.bApplies)
            {
                Advice.Add(MakeShared<FJsonValueString>(Note.Text));
            }
        }
        return Advice;
    }
}

// Actor queries

// MCP:get_actor_components - List every component on one actor in the editor OR the live PIE world: name, class, is_active, plus mesh/light details. Accepts world='editor'|'pie'|'server'|'client'|'pie:N' ('server' = the PIE world with authority, 'client' = the other one) and stamps world_name/world_package/net_mode/pie_instance so a co-op reading says WHICH world it came from. Use this to discover component names; then get_component_properties (also world-aware) for their values -- reading with the default world='editor' returns the pre-BeginPlay archetype, not the running actor.
//   @param actor_name - Actor's internal name (GetName) or editor label (GetActorLabel); exact match, not a substring.
//   @param world - Optional world selector: absent/'editor' (default) | 'pie' (ambiguous: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie').
//   @returns { actor_name: string, actor_class: string, world: string, components: [{ name: string, class: string, is_active: bool, is_editable: bool, type?: string, mesh_name?: string, intensity?: number, light_color?: string }], components_count: number, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }

TSharedPtr<FJsonObject> HandleGetActorComponents(const TSharedPtr<FJsonObject>& Params)
{
    // 'world' chooses the editor world (default) or a live PIE world.
    FString TargetName;
    FString WorldLabel;
    UWorld* World = nullptr;
    TSharedPtr<FJsonObject> Failure;
    AActor* Target = UEAgentLevelCommandsImpl::ResolveActorForQuery(Params, TargetName, World, WorldLabel, Failure);
    if (Target == nullptr)
    {
        return Failure;
    }

    TArray<TSharedPtr<FJsonValue>> ComponentRows;
    for (UActorComponent* Component : Target->GetComponents())
    {
        if (Component != nullptr)
        {
            ComponentRows.Add(MakeShared<FJsonValueObject>(UEAgentLevelCommandsImpl::DescribeComponent(Component)));
        }
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actor_name"), TargetName);
    Payload->SetStringField(TEXT("actor_class"), Target->GetClass()->GetName());
    Payload->SetStringField(TEXT("world"), WorldLabel);
    Payload->SetArrayField(TEXT("components"), ComponentRows);
    Payload->SetNumberField(TEXT("components_count"), ComponentRows.Num());
    // Which world was read -- with two PIE instances, "pie" alone does not say.
    mcp_common::AddWorldEvidence(Payload, World);
    return Payload;
}

// MCP:get_actor_blueprint - Tell whether a placed actor comes from a Blueprint and, if so, name its generated class and source asset. Cheap one-actor answer to "what BP is this thing?" -- for the reverse (all actors of a class) use get_actors_by_class. World-aware: world='editor' (default) | 'pie' | 'server' | 'client' | 'pie:N', so a RUNTIME-SPAWNED actor -- which never exists in the editor world, where it reads as a flat "Actor not found" -- can be identified too; the reply stamps world_name/world_package/net_mode/pie_instance.
//   @param actor_name - Actor's internal name (GetName) or editor label (GetActorLabel); exact match.
//   @param world - Optional world selector: absent/'editor' (default) | 'pie' (ambiguous: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie'). An unrecognised value FAILS naming the worlds that exist; it never falls back to another world in silence.
//   @returns { actor_name: string, actor_class: string, world: string, is_blueprint: bool, blueprint_class?: string, blueprint_asset?: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }

TSharedPtr<FJsonObject> HandleGetActorBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    // 'world' chooses the editor world (default) or a live PIE world, so runtime-spawned actors resolve too.
    FString TargetName;
    FString WorldLabel;
    UWorld* World = nullptr;
    TSharedPtr<FJsonObject> Failure;
    AActor* Target = UEAgentLevelCommandsImpl::ResolveActorForQuery(Params, TargetName, World, WorldLabel, Failure);
    if (Target == nullptr)
    {
        return Failure;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actor_name"), TargetName);
    Payload->SetStringField(TEXT("actor_class"), Target->GetClass()->GetName());
    Payload->SetStringField(TEXT("world"), WorldLabel);
    // Both "not found" and a Blueprint name are answers relative to one world -- stamp which.
    mcp_common::AddWorldEvidence(Payload, World);

    // An actor spawned from a Blueprint runs as its generated class; ClassGeneratedBy is the asset.
    const UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Target->GetClass());
    Payload->SetBoolField(TEXT("is_blueprint"), GeneratedClass != nullptr);
    if (GeneratedClass != nullptr)
    {
        Payload->SetStringField(TEXT("blueprint_class"), GeneratedClass->GetName());
        if (GeneratedClass->ClassGeneratedBy != nullptr)
        {
            Payload->SetStringField(TEXT("blueprint_asset"), GeneratedClass->ClassGeneratedBy->GetName());
        }
    }
    return Payload;
}

// MCP:get_actors_in_radius - Spatial query: every actor whose ORIGIN (not its bounds) lies within radius cm of a world point, with each one's distance and location. Pick this over get_actors_by_class when the question is "what is near here" instead of "where are the X". World-aware: world='editor' (default) | 'pie' | 'server' | 'client' | 'pie:N', so actors SPAWNED AT RUNTIME -- which the editor world never contains -- are reachable, and the reply stamps world_name/world_package/net_mode/pie_instance so actors_count:0 reads as "nothing near here IN THIS WORLD" instead of "nothing near here".
//   @param center - World-space center, ARRAY form only: [x, y, z]. Omitted, it is the origin [0,0,0], which reads exactly like "nothing is near there"; any other value (an object, another length) is an error.
//   @param radius - Search radius in centimetres (Unreal units), measured to each actor's origin.
//   @param world - Optional world selector: absent/'editor' (default) | 'pie' (ambiguous: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie'). An unrecognised value FAILS naming the worlds that exist; it never falls back to another world in silence.
//   @returns { center_x: number, center_y: number, center_z: number, radius: number, world: string, actors: [{ name: string, class: string, distance: number, location: { x: number, y: number, z: number } }], actors_count: number, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }

TSharedPtr<FJsonObject> HandleGetActorsInRadius(const TSharedPtr<FJsonObject>& Params)
{
    // `center` is read first; a missing or malformed one becomes the origin.
    const FVector Origin = mcp_common::ReadJsonVector(Params, TEXT("center"));
    float SearchRadius = 0.0f;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("radius"), SearchRadius)) { return Missing; }

    // Without a world selector a proximity query cannot see anything spawned after BeginPlay, which
    // reads as a confident zero; 'world' reaches the live PIE worlds.
    FString WorldLabel;
    TSharedPtr<FJsonObject> Failure;
    UWorld* World = UEAgentLevelCommandsImpl::ResolveWorldForQuery(Params, WorldLabel, Failure, TEXT("No world found"));
    if (World == nullptr)
    {
        return Failure;
    }

    TArray<TSharedPtr<FJsonValue>> Nearby;
    UEAgentLevelCommandsImpl::ForEachActorWithinRadius(World, Origin, SearchRadius,
        [&Nearby](AActor* Actor, const FVector& Where, float Distance)
        {
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("name"), Actor->GetName());
            Row->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
            Row->SetNumberField(TEXT("distance"), Distance);
            Row->SetObjectField(TEXT("location"), mcp_common::VectorToJsonObject(Where));
            Nearby.Add(MakeShared<FJsonValueObject>(Row));
        });

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    UEAgentLevelCommandsImpl::WriteFlatXYZ(Payload, TEXT("center_"), Origin);
    Payload->SetNumberField(TEXT("radius"), SearchRadius);
    Payload->SetStringField(TEXT("world"), WorldLabel);
    Payload->SetArrayField(TEXT("actors"), Nearby);
    Payload->SetNumberField(TEXT("actors_count"), Nearby.Num());
    // actors_count 0 means "nothing near here in THIS world" -- the world is stamped here.
    mcp_common::AddWorldEvidence(Payload, World);
    return Payload;
}

// MCP:get_actors_by_class - Census of one class in a chosen world: names + locations of every actor whose class name matches EXACTLY (no subclasses; a Blueprint needs its runtime name with the _C suffix, e.g. BP_Foo_C), capped at 1000. The co-op workhorse -- world='editor'|'pie'|'server'|'client'|'pie:N' ('server' = the PIE world with authority, 'client' = the other), and the reply stamps world_name/world_package/net_mode/pie_instance so actors_count:0 is readable as "not in THIS world" instead of "does not exist". Use find_actors_by_name for substring/label search, get_actors_in_radius for proximity. Two false zeros: with 2+ PIE clients a bare world='pie' resolves to only ONE of the two worlds; and after compile_blueprint the placed OFPA instances vanish from BOTH editor and PIE worlds in memory (git shows them intact on disk -- the fix is load_level of the same map, never re-placing them).
//   @param class_name - Whole UClass name, e.g. 'StaticMeshActor' or 'BP_MyTurret_C'. Compared case-insensitively but WHOLE-string: not a path, not a substring, and never matched against parent classes.
//   @param world - Optional world selector: absent/'editor' (default) | 'pie' (ambiguous: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie').
//   @returns { class_name: string, world: string, actors: [{ name: string, class: string, location: { x: number, y: number, z: number } }], actors_count: number, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }

TSharedPtr<FJsonObject> HandleGetActorsByClass(const TSharedPtr<FJsonObject>& Params)
{
    FString WantedClass;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("class_name"), WantedClass, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // 'world' chooses the editor world (default) or a live PIE world.
    FString WorldLabel;
    TSharedPtr<FJsonObject> Failure;
    UWorld* World = UEAgentLevelCommandsImpl::ResolveWorldForQuery(Params, WorldLabel, Failure, TEXT("No world found"));
    if (World == nullptr)
    {
        return Failure;
    }

    TArray<TSharedPtr<FJsonValue>> Rows;
    for (AActor* Match : CollectActorsOfClass(World, WantedClass))
    {
        if (Match == nullptr)
        {
            continue;
        }
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Match->GetName());
        Row->SetStringField(TEXT("class"), Match->GetClass()->GetName());
        Row->SetObjectField(TEXT("location"), mcp_common::VectorToJsonObject(Match->GetActorLocation()));
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("class_name"), WantedClass);
    Payload->SetStringField(TEXT("world"), WorldLabel);
    Payload->SetArrayField(TEXT("actors"), Rows);
    Payload->SetNumberField(TEXT("actors_count"), Rows.Num());
    // actors_count 0 means "not in THIS world" -- the world is stamped here.
    mcp_common::AddWorldEvidence(Payload, World);
    return Payload;
}

// MCP:get_lighting_in_area - Light-only spatial query: every actor carrying a LightComponent within radius cm of a world point, reporting light_type, intensity, color and distance. Use it to answer "why is this spot dark / blown out" without walking the whole outliner (get_actors_in_radius returns everything and no light data). World-aware: world='editor' (default) | 'pie' | 'server' | 'client' | 'pie:N', which is what lets you see lights attached to actors that only exist at runtime (a torch on a spawned character, a muzzle flash), and the reply stamps world_name/world_package/net_mode/pie_instance so lights_count:0 is readable as "not in THIS world".
//   @param center - World-space center, ARRAY form only: [x, y, z]. Omitted, it is the origin [0,0,0], which reads exactly like "there are no lights here"; any other value (an object, another length) is an error.
//   @param radius - Search radius in centimetres (Unreal units), measured to each light actor's origin.
//   @param world - Optional world selector: absent/'editor' (default) | 'pie' (ambiguous: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie'). An unrecognised value FAILS naming the worlds that exist; it never falls back to another world in silence.
//   @returns { center_x: number, center_y: number, center_z: number, radius: number, world: string, lights: [{ actor_name: string, light_type: string, intensity: number, light_color: string, distance: number, location: { x: number, y: number, z: number } }], lights_count: number, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }

TSharedPtr<FJsonObject> HandleGetLightingInArea(const TSharedPtr<FJsonObject>& Params)
{
    // `center` is read first; a missing or malformed one becomes the origin.
    const FVector Origin = mcp_common::ReadJsonVector(Params, TEXT("center"));
    float SearchRadius = 0.0f;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("radius"), SearchRadius)) { return Missing; }

    // 'world' chooses the editor world (default) or a live PIE world.
    FString WorldLabel;
    TSharedPtr<FJsonObject> Failure;
    UWorld* World = UEAgentLevelCommandsImpl::ResolveWorldForQuery(Params, WorldLabel, Failure, TEXT("No world found"));
    if (World == nullptr)
    {
        return Failure;
    }

    // Only actors carrying a light component are listed, described by their first one.
    TArray<TSharedPtr<FJsonValue>> LightRows;
    UEAgentLevelCommandsImpl::ForEachActorWithinRadius(World, Origin, SearchRadius,
        [&LightRows](AActor* Actor, const FVector& Where, float Distance)
        {
            const ULightComponent* Light = Actor->FindComponentByClass<ULightComponent>();
            if (Light == nullptr)
            {
                return;
            }
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("actor_name"), Actor->GetName());
            Row->SetStringField(TEXT("light_type"), Light->GetClass()->GetName());
            Row->SetNumberField(TEXT("intensity"), Light->Intensity);
            Row->SetStringField(TEXT("light_color"), Light->LightColor.ToString());
            Row->SetNumberField(TEXT("distance"), Distance);
            Row->SetObjectField(TEXT("location"), mcp_common::VectorToJsonObject(Where));
            LightRows.Add(MakeShared<FJsonValueObject>(Row));
        });

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    UEAgentLevelCommandsImpl::WriteFlatXYZ(Payload, TEXT("center_"), Origin);
    Payload->SetNumberField(TEXT("radius"), SearchRadius);
    Payload->SetStringField(TEXT("world"), WorldLabel);
    Payload->SetArrayField(TEXT("lights"), LightRows);
    Payload->SetNumberField(TEXT("lights_count"), LightRows.Num());
    // lights_count 0 means "no lights near here in THIS world" -- the world is stamped here.
    mcp_common::AddWorldEvidence(Payload, World);
    return Payload;
}

// World settings and game mode

// MCP:get_world_settings - Read a world's framework wiring: default GameMode plus the pawn / PlayerController / HUD / spectator classes it hands out, and gravity, time dilation and paused state. Use this to check WHICH GameMode+PC a map will spawn. It also reads the physics/bounds side: the authored GlobalGravityZ, its override switch and the effective gravity, KillZ and its damage type, whether world-bounds checks are on (KillZ does nothing without them), WorldToMeters and bForceNoPrecomputedLighting -- the read-back of set_world_gravity and set_kill_z, which write the editor world. World-aware: with the default world='editor' there is no live GameMode, so the classes come from the DefaultGameMode CDO -- i.e. what the map INTENDS; ask for world='server' and GetAuthGameMode() returns the GameMode a running PIE ACTUALLY instantiated, which is the read that settles a set_player_controller_class that looks applied and is not. WARNING: world_type is stamped by the canonical world evidence, so it can read 'PIE', not only Game/Editor/Other.
//   @param world - Optional world selector: absent/'editor' (default) | 'pie' (ambiguous: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority -- the only one with a live AuthGameMode) | 'client' | 'pie:N' | 'game' (alias of 'pie'). An unrecognised value FAILS naming the worlds that exist; it never falls back to another world in silence.
//   @returns { world: string, world_name: string, world_type: string, default_game_mode: string, default_pawn_class: string, player_controller_class: string, hud_class: string, spectator_class: string, world_gravity_enabled: bool (the effective gravity Z is non-zero), world_gravity_overridden: bool (Override World Gravity), world_gravity_z: number, global_gravity_z: number (the authored override value), global_gravity_set: bool (Override World Gravity), effective_gravity_z: number (WorldSettings GetGravityZ), kill_z: number, kill_z_damage_type: string, enable_world_bounds_checks: bool, world_to_meters: number, force_no_precomputed_lighting: bool, time_dilation: number, paused: number, world_package: string|null, net_mode: string|null, pie_instance: number }

TSharedPtr<FJsonObject> HandleGetWorldSettings(const TSharedPtr<FJsonObject>& Params)
{
    // GetAuthGameMode() is null in the editor world by construction: the editor world answers "what
    // this map intends" (the DefaultGameMode CDO), a server PIE world "what this session spawned".
    FString WorldLabel;
    TSharedPtr<FJsonObject> Failure;
    UWorld* World = UEAgentLevelCommandsImpl::ResolveWorldForQuery(Params, WorldLabel, Failure, TEXT("No valid world found"), /*bRequireValidLowLevel=*/true);
    if (World == nullptr)
    {
        return Failure;
    }
    AWorldSettings* Settings = World->GetWorldSettings();
    if (Settings == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("WorldSettings not found"));
    }

    // The pawn / controller / HUD / spectator classes come from the live authority GameMode when
    // there is one, otherwise from the class defaults of DefaultGameMode.
    UClass* ModeClass = Settings->DefaultGameMode;
    AGameModeBase* ClassSource = World->GetAuthGameMode();
    if (ClassSource == nullptr && ModeClass != nullptr && ModeClass->IsChildOf(AGameModeBase::StaticClass()))
    {
        ClassSource = ModeClass->GetDefaultObject<AGameModeBase>();
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("world_name"), World->GetName());
    Payload->SetStringField(TEXT("default_game_mode"), UEAgentLevelCommandsImpl::ClassNameOrNone(ModeClass));
    Payload->SetStringField(TEXT("default_pawn_class"),
        UEAgentLevelCommandsImpl::ClassNameOrNone(ClassSource ? ClassSource->DefaultPawnClass.Get() : nullptr));
    Payload->SetStringField(TEXT("player_controller_class"),
        UEAgentLevelCommandsImpl::ClassNameOrNone(ClassSource ? ClassSource->PlayerControllerClass.Get() : nullptr));
    Payload->SetStringField(TEXT("hud_class"),
        UEAgentLevelCommandsImpl::ClassNameOrNone(ClassSource ? ClassSource->HUDClass.Get() : nullptr));
    Payload->SetStringField(TEXT("spectator_class"),
        UEAgentLevelCommandsImpl::ClassNameOrNone(ClassSource ? ClassSource->SpectatorClass.Get() : nullptr));
    // Gravity acts when the effective Z is non-zero. bWorldGravitySet only says the value has
    // been cached at runtime; the override switch is bGlobalGravitySet.
    const float GravityZ = World->GetGravityZ();
    Payload->SetBoolField(TEXT("world_gravity_enabled"), GravityZ != 0.0f);
    Payload->SetBoolField(TEXT("world_gravity_overridden"), Settings->bGlobalGravitySet);
    Payload->SetNumberField(TEXT("world_gravity_z"), GravityZ);
    // The authored override value, its switch, and the gravity the settings really hand out.
    Payload->SetNumberField(TEXT("global_gravity_z"), Settings->GlobalGravityZ);
    Payload->SetBoolField(TEXT("global_gravity_set"), Settings->bGlobalGravitySet != 0);
    Payload->SetNumberField(TEXT("effective_gravity_z"), Settings->GetGravityZ());
    // Death plane and world bounds: KillZ only acts while the bounds checks are on.
    Payload->SetNumberField(TEXT("kill_z"), Settings->KillZ);
    Payload->SetStringField(TEXT("kill_z_damage_type"), UEAgentLevelCommandsImpl::ClassNameOrNone(Settings->KillZDamageType.Get()));
    Payload->SetBoolField(TEXT("enable_world_bounds_checks"), Settings->bEnableWorldBoundsChecks != 0);
    // Scale and lighting.
    Payload->SetNumberField(TEXT("world_to_meters"), Settings->WorldToMeters);
    Payload->SetBoolField(TEXT("force_no_precomputed_lighting"), Settings->bForceNoPrecomputedLighting);
    Payload->SetNumberField(TEXT("time_dilation"), Settings->TimeDilation);
    Payload->SetNumberField(TEXT("paused"), Settings->GetPauserPlayerState() != nullptr ? 1.0 : 0.0);

    // The evidence stamps world_type with the canonical EWorldType name ("Editor", "PIE",
    // "Game"...) and repeats world_name with the same string.
    Payload->SetStringField(TEXT("world"), WorldLabel);
    mcp_common::AddWorldEvidence(Payload, World);
    return Payload;
}

// MCP:set_player_controller_class - Point the world's GameMode at a PlayerController class, writing the live GameMode instance if one exists and otherwise the defaults of the world's Blueprint GameMode (refused when the GameMode is a native class, or when the world has none: those defaults are process-wide and never saved). success is read back from the slot written. WARNING: It does NOT compile or save the GameMode Blueprint, so a later PIE can still instantiate the OLD class from the already-compiled BlueprintGeneratedClass (three reads can agree on the new class while PIE spawns the engine default). Always follow with compile_blueprint on the GameMode and then save_asset, and verify in PIE by the SPAWNED controller's full class path -- never by re-reading this slot.
//   @param player_controller_class - Class name or full path (e.g. '/Game/MyGame/Core/BP_MyPlayerController.BP_MyPlayerController_C'); rejected if it is not a child of APlayerController.
//   @returns { player_controller_class: string, game_mode_class: string, written_to: string ('live_game_mode' | 'game_mode_defaults'), success: bool (the written slot reads back the class; still not a PIE oracle) }

TSharedPtr<FJsonObject> HandleSetPlayerControllerClass(const TSharedPtr<FJsonObject>& Params)
{
    FString RequestedClass;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("player_controller_class"), RequestedClass, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // The first world context of the engine (the editor world in the editor).
    UWorld* World = mcp_common::GetFirstContextWorld();
    if (World == nullptr || !World->IsValidLowLevel())
    {
        return mcp_common::MakeErrorReply(TEXT("No valid world found"));
    }
    AWorldSettings* Settings = World->GetWorldSettings();
    if (Settings == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("WorldSettings not found"));
    }

    UClass* ControllerClass = UEAgentLevelCommandsImpl::FindOrLoadClass<APlayerController>(RequestedClass);
    if (ControllerClass == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("PlayerController class not found: %s. Try using full path or class name."), *RequestedClass));
    }
    if (!ControllerClass->IsChildOf(APlayerController::StaticClass()))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Class is not a PlayerController: %s"), *RequestedClass));
    }

    // The controller class belongs to the GameMode, not to AWorldSettings: the running authority
    // GameMode when there is one, else the defaults of the world's Blueprint GameMode. The defaults
    // of a NATIVE GameMode class (AGameModeBase included) are shared by the whole process and never
    // saved, so they are not written.
    AGameModeBase* LiveMode = World->GetAuthGameMode();
    UClass* ModeClass = LiveMode != nullptr ? LiveMode->GetClass() : Settings->DefaultGameMode.Get();
    UBlueprint* ModeBlueprint = ModeClass != nullptr ? Cast<UBlueprint>(ModeClass->ClassGeneratedBy) : nullptr;
    if (LiveMode == nullptr && ModeBlueprint == nullptr)
    {
        return mcp_common::MakeErrorReply(ModeClass == nullptr
            ? FString(TEXT("This world has no GameMode override: set a Blueprint GameMode first (set_game_mode_override)."))
            : FString::Printf(TEXT("The world's GameMode %s is a native class: its defaults are process-wide and never saved. Use a Blueprint GameMode."), *ModeClass->GetName()));
    }

    AGameModeBase* Target = LiveMode != nullptr ? LiveMode : ModeClass->GetDefaultObject<AGameModeBase>();
    {
        const FScopedTransaction UndoScope(FText::FromString(TEXT("Set Player Controller Class")));
        // Modify() records the change and dirties the package, so save_asset writes the defaults.
        Target->Modify();
        Target->PlayerControllerClass = ControllerClass;
    }
    const bool bReadBack = Target->PlayerControllerClass.Get() == ControllerClass;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("player_controller_class"), RequestedClass);
    Payload->SetStringField(TEXT("game_mode_class"), ModeClass->GetPathName());
    Payload->SetStringField(TEXT("written_to"), LiveMode != nullptr ? TEXT("live_game_mode") : TEXT("game_mode_defaults"));
    Payload->SetBoolField(TEXT("success"), bReadBack);
    if (!bReadBack)
    {
        Payload->SetStringField(TEXT("error"), TEXT("set_player_controller_class: the GameMode does not read back the requested class."));
    }
    return Payload;
}

namespace
{
    // Build a REAL cube brush of HalfExtent on an AVolume, the way the editor's volume actor factories do.
    // Returns true if a non-empty UModel brush ended up on the volume.
    bool BuildCubeBrushForVolume_Level(AVolume* VolumeActor, const FVector& HalfExtent)
    {
        if (!VolumeActor)
        {
            return false;
        }
        mcp_common::BuildCubeBrushForVolume(VolumeActor, HalfExtent);
        return VolumeActor->Brush != nullptr
            && VolumeActor->Brush->Polys != nullptr
            && VolumeActor->Brush->Polys->Element.Num() > 0;
    }
}

// Navmesh

// MCP:spawn_navmesh_bounds_volume - Spawn a NavMeshBoundsVolume in the editor world at location with a REAL cube brush of half-extent extent (default 1000,1000,500) -- an empty brush yields 0 navmesh tiles -- and report brush and bounds RE-READ off the actor in diagnose_navmesh's terms (brush_empty, brush_poly_count, bounds_valid). spawn_box_volume(volume_class='NavMeshBoundsVolume') builds the same brush plus a label, spatial-loading flag and scale normalization; this one differs by its larger default and report fields. One undo step; not saved (save_level); navigation is not rebuilt here.
//   @retired spawn_box_volume - volume_class="NavMeshBoundsVolume", extent=[1000,1000,500]
//   @param location - Optional [X, Y, Z] world location (defaults to origin)
//   @param extent - Optional [X, Y, Z] **SEMI-EXTENT** (half-extent) in cm (defaults to [1000, 1000, 500]). The built cube's FULL edge length is 2x this on every axis -- e.g. extent [1000, 1000, 500] yields a 2000 x 2000 x 1000 cube. Echoed back under `requested.extent`; what the actor ended up with is `bounds_extent_actual`.
//   @returns { actor_name: string, actor_path: string, location_x: number, location_y: number, location_z: number, extent_x: number, extent_y: number, extent_z: number, brush_built: bool, has_brush_geometry: bool, brush_present: bool, brush_poly_count: number (-1 = no UModel/no editor-only Polys data), brush_empty: bool, bounds_valid: bool, bounds_extent_x: number, bounds_extent_y: number, bounds_extent_z: number, bounds_extent_actual: { x, y, z }, bounds_origin_actual: { x, y, z }, actor_scale: { x, y, z }, location_actual: { x, y, z }, requested: { extent }, success: bool }

TSharedPtr<FJsonObject> HandleSpawnNavMeshBoundsVolume(const TSharedPtr<FJsonObject>& Params)
{
    // Both inputs are optional [x, y, z] arrays; extent is a HALF-extent.
    const FVector SpawnAt = Params->HasField(TEXT("location"))
        ? mcp_common::ReadJsonVector(Params, TEXT("location"))
        : FVector(0, 0, 0);
    const FVector HalfExtent = Params->HasField(TEXT("extent"))
        ? mcp_common::ReadJsonVector(Params, TEXT("extent"))
        : FVector(1000, 1000, 500);

    UWorld* EditorWorld = mcp_common::GetEditorWorld();
    if (EditorWorld == nullptr || !EditorWorld->IsValidLowLevel())
    {
        return mcp_common::MakeErrorReply(TEXT("No valid world found"));
    }
    if (GEditor == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("GEditor not available"));
    }

    const FScopedTransaction UndoScope(FText::FromString(TEXT("Spawn NavMesh Bounds Volume")));
    ANavMeshBoundsVolume* Volume = EditorWorld->SpawnActor<ANavMeshBoundsVolume>(SpawnAt, FRotator::ZeroRotator);
    if (Volume == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn NavMeshBoundsVolume"));
    }

    // A brush volume spawns with an EMPTY model: zero nav bounds, hence zero navmesh tiles. Build a
    // real cube brush from the half-extent (the builder takes full edge lengths), then put the actor
    // back where it was asked to be, since building the brush runs edit-change hooks that can move it.
    const bool bBrushBuilt = BuildCubeBrushForVolume_Level(Cast<AVolume>(Volume), HalfExtent);
    Volume->SetActorLocation(SpawnAt);

    // Re-read the brush the actor actually ended up with and report THAT, using the same fields
    // diagnose_navmesh reports (brush_empty / brush_poly_count / bounds_valid). The verdict comes
    // from this reading; brush_built is still emitted for compatibility, but it is not the verdict.
    int32 PolyCount = -1;   // -1: there is no UModel, or no editor-only poly data to count
    bool bHasModel = false;
    if (const ABrush* AsBrush = Cast<ABrush>(Volume))
    {
        if (const UModel* Model = AsBrush->Brush)
        {
            bHasModel = true;
            if (Model->Polys)
            {
                PolyCount = Model->Polys->Element.Num();
            }
        }
    }
    const bool bHasGeometry = bHasModel && PolyCount > 0;

    // World-space bounds of the actor as it ended up; an unbuilt brush gives degenerate bounds.
    const FBox Bounds = Volume->GetComponentsBoundingBox(/*bNonColliding=*/true);
    const FVector BoundsExtent = Bounds.IsValid ? Bounds.GetExtent() : FVector::ZeroVector;
    const FVector BoundsCenter = Bounds.IsValid ? Bounds.GetCenter() : FVector::ZeroVector;
    const bool bBoundsValid = (Bounds.IsValid != 0) && (BoundsExtent.SizeSquared() > 1.0);
    const bool bBrushEmpty = !bBoundsValid || PolyCount == 0;
    const bool bReallyBuilt = bHasGeometry && bBoundsValid;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actor_name"), Volume->GetName());
    Payload->SetStringField(TEXT("actor_path"), Volume->GetPathName());
    UEAgentLevelCommandsImpl::WriteFlatXYZ(Payload, TEXT("location_"), SpawnAt);
    UEAgentLevelCommandsImpl::WriteFlatXYZ(Payload, TEXT("extent_"), HalfExtent);
    Payload->SetBoolField(TEXT("brush_built"), bBrushBuilt);
    Payload->SetBoolField(TEXT("has_brush_geometry"), bHasGeometry);
    Payload->SetBoolField(TEXT("brush_present"), bHasModel);
    Payload->SetNumberField(TEXT("brush_poly_count"), PolyCount);
    Payload->SetBoolField(TEXT("brush_empty"), bBrushEmpty);
    Payload->SetBoolField(TEXT("bounds_valid"), bBoundsValid);
    UEAgentLevelCommandsImpl::WriteFlatXYZ(Payload, TEXT("bounds_extent_"), BoundsExtent);
    Payload->SetObjectField(TEXT("bounds_extent_actual"), mcp_common::VectorToJsonObject(BoundsExtent));
    Payload->SetObjectField(TEXT("bounds_origin_actual"), mcp_common::VectorToJsonObject(BoundsCenter));
    // A residual actor scale is the difference between the extent asked for and the one the actor has.
    Payload->SetObjectField(TEXT("actor_scale"), mcp_common::VectorToJsonObject(Volume->GetActorScale3D()));
    Payload->SetObjectField(TEXT("location_actual"), mcp_common::VectorToJsonObject(Volume->GetActorLocation()));

    // The request itself, marked as INPUT under "requested" (the flat extent_* fields are kept too).
    mcp_common::AddRequestedEcho(Payload, Params, { TEXT("extent"), TEXT("location") });

    FString OutcomeError;
    if (!bReallyBuilt)
    {
        OutcomeError = FString::Printf(
            TEXT("spawn_navmesh_bounds_volume: the volume '%s' was spawned but its brush is EMPTY/degenerate ")
            TEXT("(brush_present=%s, brush_poly_count=%d, bounds_valid=%s, bounds_extent=[%g, %g, %g]). ")
            TEXT("An empty brush contributes NO navigable region -> 0 navmesh tiles. Verify with diagnose_navmesh."),
            *Volume->GetName(),
            bHasModel ? TEXT("true") : TEXT("false"),
            PolyCount,
            bBoundsValid ? TEXT("true") : TEXT("false"),
            BoundsExtent.X, BoundsExtent.Y, BoundsExtent.Z);
    }

    // `success` follows the re-read above, not the fact that this line was reached.
    return mcp_common::MakeOutcomeReply(Payload, bReallyBuilt, OutcomeError);
}

// MCP:get_navigation_data - Inventory of navigation in a chosen world: every ANavigationData actor (the RecastNavMesh included) with its class, every NavMeshBoundsVolume with its location, and whether the world is flagged whole-world-navigable. The first thing to read when AI will not path -- no bounds volume, or a volume with an empty brush, means zero navmesh tiles. Errors honestly if the world has no navigation system at all. World-aware: world='editor' (default) | 'server' | 'client' | 'pie:N' | 'pie'. WARNING: The navmesh is SERVER-AUTHORITATIVE: asking a client answers "Navigation System not found" while the server's navmesh is perfectly alive, so anchor world='server' -- and the reply stamps world_name/world_package/net_mode/pie_instance so you can prove which one you asked.
//   @param world - Optional world selector: absent/'editor' (default) | 'server' (the PIE world with authority -- the ONLY one that owns a navmesh) | 'client' | 'pie:N' | 'pie' (ambiguous) | 'game' (alias of 'pie'). An unrecognised value FAILS naming the worlds that exist; it never falls back to another world in silence.
//   @returns { world: string, navigation_data: [{ name: string, class: string }], navigation_data_count: number, nav_bounds_volumes: [{ name: string, location_x: number, location_y: number, location_z: number }], nav_bounds_volumes_count: number, navigation_system_available: bool, is_whole_world_navigable: bool, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }

TSharedPtr<FJsonObject> HandleGetNavigationData(const TSharedPtr<FJsonObject>& Params)
{
    // 'world' chooses the editor world (default; the resolver's 'editor' branch is the editor
    // world context) or a live PIE world. "AI will not path" is usually a PIE question, and only
    // the authoritative side owns a navmesh.
    FString WorldLabel;
    TSharedPtr<FJsonObject> Failure;
    UWorld* World = UEAgentLevelCommandsImpl::ResolveWorldForQuery(Params, WorldLabel, Failure, TEXT("No valid world found"), /*bRequireValidLowLevel=*/true);
    if (World == nullptr)
    {
        return Failure;
    }

    UNavigationSystemV1* Navigation = Cast<UNavigationSystemV1>(World->GetNavigationSystem());
    if (Navigation == nullptr)
    {
        Navigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
    }
    if (Navigation == nullptr)
    {
        // On a CLIENT world this is the expected answer (the navmesh is server-authoritative),
        // so the error names the world and carries its identity too.
        TSharedPtr<FJsonObject> NoNavigation = mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Navigation System not found (world='%s'). The navmesh is server-authoritative: on a CLIENT world this is the expected answer -- retry with world='server'."),
            *WorldLabel));
        mcp_common::AddWorldEvidence(NoNavigation, World);
        return NoNavigation;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("world"), WorldLabel);
    // Zero nav data / zero bounds volumes is an answer about one world -- stamp which.
    mcp_common::AddWorldEvidence(Payload, World);

    // Every navigation data actor of the world (the RecastNavMesh among them).
    TArray<TSharedPtr<FJsonValue>> DataRows;
    for (TActorIterator<ANavigationData> It(World); It; ++It)
    {
        const ANavigationData* Data = *It;
        if (Data == nullptr)
        {
            continue;
        }
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Data->GetName());
        Row->SetStringField(TEXT("class"), Data->GetClass()->GetName());
        DataRows.Add(MakeShared<FJsonValueObject>(Row));
    }
    Payload->SetArrayField(TEXT("navigation_data"), DataRows);
    Payload->SetNumberField(TEXT("navigation_data_count"), DataRows.Num());

    // Every bounds volume; the count is of the volumes walked.
    TArray<TSharedPtr<FJsonValue>> VolumeRows;
    int32 VolumesWalked = 0;
    for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It)
    {
        ++VolumesWalked;
        const AActor* BoundsVolume = *It;
        if (BoundsVolume == nullptr)
        {
            continue;
        }
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), BoundsVolume->GetName());
        UEAgentLevelCommandsImpl::WriteFlatXYZ(Row, TEXT("location_"), BoundsVolume->GetActorLocation());
        VolumeRows.Add(MakeShared<FJsonValueObject>(Row));
    }
    Payload->SetArrayField(TEXT("nav_bounds_volumes"), VolumeRows);
    Payload->SetNumberField(TEXT("nav_bounds_volumes_count"), VolumesWalked);

    // bAutoCreateNavigationData is protected and is therefore not reported.
    Payload->SetBoolField(TEXT("navigation_system_available"), Navigation != nullptr);
    Payload->SetBoolField(TEXT("is_whole_world_navigable"), Navigation->bWholeWorldNavigable);
    return Payload;
}

// ---------------------------------------------------------------------------------------------
// Play-in-Editor session commands. State is read through ULevelEditorSubsystem; sessions are
// requested from GEditor.
// ---------------------------------------------------------------------------------------------

// What is UNSAVED when Play is pressed.
//
// This is an ADVISORY, never a gate: starting PIE must not fail because something is dirty. It exists
// because a PIE session that kills the editor takes exactly the unsaved work with it. Same family as
// the PIE-safety guard (which refuses, naming its cause and its fix), one notch softer: it reports
// instead of refusing.
namespace McpPieDirtyAdvisory
{
    // Stamps dirty_package_count / readonly_dirty_count / dirty_packages[] / dirty_packages_truncated
    // onto Response. Emitted on EVERY start_play_in_editor path, including the already_in_pie
    // early-out: a field that is sometimes absent forces the caller to tell "missing" from "zero".
    static void Stamp(const TSharedPtr<FJsonObject>& Response)
    {
        // Same two statics list_dirty_packages uses (Editor/UEAgentSystemCommands.cpp): the
        // union of world-owned (map + MapBuildData + OFPA externals) and content packages.
        TArray<UPackage*> DirtyPackages;
        FEditorFileUtils::GetDirtyWorldPackages(DirtyPackages);
        FEditorFileUtils::GetDirtyContentPackages(DirtyPackages);

        // A dirty MAP drags hundreds of OFPA external actor packages with it, so the list is capped
        // while the COUNT stays exact -- the count is what tells you whether to go and save.
        static constexpr int32 MaxListed = 25;

        TSet<UPackage*> Seen;
        TArray<TSharedPtr<FJsonValue>> Listed;
        int32 NumDirty = 0;
        int32 NumReadOnlyDirty = 0;

        for (UPackage* Package : DirtyPackages)
        {
            if (!Package || Seen.Contains(Package))
            {
                continue;
            }
            Seen.Add(Package);

            const FString PackageName = Package->GetName();
            ++NumDirty;
            // Dirty under a read-only root (e.g. /Game/VendorPack) is its own alarm: it means something
            // edited a vendored library in place, which read-only roots exist to forbid.
            if (mcp_common::IsUnderReadOnlyRoot(PackageName))
            {
                ++NumReadOnlyDirty;
            }
            if (Listed.Num() < MaxListed)
            {
                Listed.Add(MakeShared<FJsonValueString>(PackageName));
            }
        }

        Response->SetNumberField(TEXT("dirty_package_count"), NumDirty);
        Response->SetNumberField(TEXT("readonly_dirty_count"), NumReadOnlyDirty);
        Response->SetArrayField(TEXT("dirty_packages"), Listed);
        Response->SetBoolField(TEXT("dirty_packages_truncated"), NumDirty > Listed.Num());
    }
}

// MCP:start_play_in_editor - Press Play: starts a real PlayInEditor session (a true play world with a spawned PlayerController, not SimulateInEditor), optionally multi-client and networked for co-op testing via number_of_clients + net_mode=ListenServer. WARNING: RequestPlaySession is ASYNC and this tool probes right after asking, so `pie_started` comes back FALSE on a start that is perfectly fine -- the oracle is a follow-up is_play_in_editor_active, never this field. With 2+ clients the worlds are UEDPIE_0 (server) and UEDPIE_1 (client), and later reads must be anchored with world='server'/'client' because PIE actor NAMES are swapped between them. Refuses if another MCP session (or a human) already owns PIE unless force=true.
//   @param number_of_clients - Optional int (default 1, clamped 1-10). >1 opens additional client windows -- that is what makes a co-op reading possible at all.
//   @param net_mode - Optional string: "Standalone"|"ListenServer"|"Client" (default "Standalone"). Any other value is a clean error, not a silent fallback. Use ListenServer for co-op.
//   @param run_under_one_process - Optional bool (default true). Must stay true for multi-client windows to open inside this single editor process.
//   @param force [bool] - Optional bool (default false). Override the PIE-ownership guard: start even if another session (or a manually-started PIE) currently owns Play-in-Editor. Read by the server before the tool runs, so a non-owner session can seize PIE.
//   @returns { pie_started: bool (probed synchronously right after an ASYNC request -- usually false even on success; confirm with is_play_in_editor_active), already_in_pie: bool, message: string, number_of_clients: int, net_mode: string, dirty_package_count: int, readonly_dirty_count: int, dirty_packages: [string] (capped at 25), dirty_packages_truncated: bool }
//   WARNING: The dirty_* fields are an ADVISORY, not a gate: they are what a PIE session that kills the editor would take with it. A non-zero dirty_package_count is the cue to save first. readonly_dirty_count > 0 is a louder alarm — something edited a vendor library in place. The fields are emitted on every path, including already_in_pie.

TSharedPtr<FJsonObject> HandleStartPlayInEditor(const TSharedPtr<FJsonObject>& Params)
{
    ULevelEditorSubsystem* EditorLevels = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
    if (EditorLevels == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Level Editor Subsystem not found"));
    }

    // Already playing: say so (with the unsaved-work advisory) instead of requesting a second session.
    if (EditorLevels->IsInPlayInEditor())
    {
        TSharedPtr<FJsonObject> AlreadyRunning = MakeShared<FJsonObject>();
        AlreadyRunning->SetBoolField(TEXT("already_in_pie"), true);
        AlreadyRunning->SetBoolField(TEXT("pie_started"), false);
        AlreadyRunning->SetStringField(TEXT("message"), TEXT("Play-in-Editor is already active"));
        McpPieDirtyAdvisory::Stamp(AlreadyRunning);
        return mcp_common::MakeOkReply(AlreadyRunning);
    }

    // Optional session shape: client count (clamped to 1-10), net mode and process model.
    int32 ClientCount = 1;
    double RequestedClients = 0.0;
    if (Params->TryGetNumberField(TEXT("number_of_clients"), RequestedClients))
    {
        ClientCount = FMath::Clamp(static_cast<int32>(RequestedClients), 1, 10);
    }
    FString NetModeText = TEXT("Standalone");
    Params->TryGetStringField(TEXT("net_mode"), NetModeText);
    EPlayNetMode NetMode = PIE_Standalone;
    if (!UEAgentLevelCommandsImpl::ParsePlayNetMode(NetModeText, NetMode))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown net_mode '%s' (expected Standalone|ListenServer|Client)"), *NetModeText));
    }
    bool bSingleProcess = true;
    Params->TryGetBoolField(TEXT("run_under_one_process"), bSingleProcess);

    FLevelEditorModule& LevelEditor = FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
    TSharedPtr<IAssetViewport> HostViewport = LevelEditor.GetFirstActiveViewport();
    if (!HostViewport.IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("No active level viewport available to start Play-in-Editor in"));
    }

    // GEditor->RequestPlaySession is called directly, not via the subsystem's EditorRequestBeginPlay():
    // only the direct call takes a per-request settings object for multi-client / net-mode sessions.
    FRequestPlaySessionParams Request;
    Request.WorldType = EPlaySessionWorldType::PlayInEditor;
    Request.DestinationSlateViewport = HostViewport;
    if (ClientCount != 1 || NetMode != PIE_Standalone || !bSingleProcess)
    {
        // A transient settings object, not the CDO: the user's own Play settings are left alone.
        ULevelEditorPlaySettings* SessionSettings = NewObject<ULevelEditorPlaySettings>();
        SessionSettings->SetPlayNumberOfClients(ClientCount);
        SessionSettings->SetPlayNetMode(NetMode);
        SessionSettings->SetRunUnderOneProcess(bSingleProcess);
        Request.EditorPlaySettings = SessionSettings;
    }
    GEditor->RequestPlaySession(Request);

    // The request is served on a later tick, so this immediate probe usually still answers false.
    const bool bRunningNow = EditorLevels->IsInPlayInEditor();

    TSharedPtr<FJsonObject> Outcome = MakeShared<FJsonObject>();
    Outcome->SetBoolField(TEXT("already_in_pie"), false);
    Outcome->SetBoolField(TEXT("pie_started"), bRunningNow);
    Outcome->SetStringField(TEXT("message"), bRunningNow
        ? TEXT("Play-in-Editor started successfully")
        : TEXT("Play-in-Editor requested (async); use wait_for_pie_completion or is_play_in_editor_active to confirm"));
    Outcome->SetNumberField(TEXT("number_of_clients"), ClientCount);
    Outcome->SetStringField(TEXT("net_mode"), NetModeText);
    // Taken after the request but before returning: nothing has been torn down yet, so this is the
    // set of unsaved packages the new session puts at risk.
    McpPieDirtyAdvisory::Stamp(Outcome);
    return mcp_common::MakeOkReply(Outcome);
}

// MCP:stop_play_in_editor - Press Stop: requests the end of the Play-in-Editor session (EditorRequestEndPlay). WARNING: `pie_stopped` is HARDCODED true whenever PIE was running and teardown is async, so the only oracle is a follow-up is_play_in_editor_active. WARNING: Treat every call as an operation that CAN KILL THE EDITOR: any UWorld/UPackage handle left in an execute_python global keeps the PIE world rooted and the engine aborts with "Object ... from PIE level still referenced" -- purge the __main__ namespace and gc.collect() BEFORE calling. Refuses when another MCP session (or a human) started the PIE, and then needs force=true.
//   @param force [bool] - Optional bool (default false). Override the PIE-ownership guard: stop even if another session (or a manually-started PIE) currently owns Play-in-Editor. Read by the server before the tool runs; without it, stopping a PIE your session does not own is refused with pie_owner/requester in the payload.
//   @returns { pie_stopped: bool (hardcoded true when PIE was up -- not a measurement), was_in_pie: bool, still_in_pie: bool (read immediately, so usually still true: teardown is async), message: string }

TSharedPtr<FJsonObject> HandleStopPlayInEditor(const TSharedPtr<FJsonObject>& Params)
{
    ULevelEditorSubsystem* EditorLevels = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
    if (EditorLevels == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Level Editor Subsystem not found"));
    }

    const bool bWasRunning = EditorLevels->IsInPlayInEditor();
    if (!bWasRunning)
    {
        TSharedPtr<FJsonObject> Idle = MakeShared<FJsonObject>();
        Idle->SetBoolField(TEXT("pie_stopped"), false);
        Idle->SetBoolField(TEXT("was_in_pie"), false);
        Idle->SetStringField(TEXT("message"), TEXT("Play-in-Editor is not active"));
        return mcp_common::MakeOkReply(Idle);
    }

    // Ending the session is asynchronous: teardown finishes on later ticks, so the state read right
    // after the request normally still shows the session running (still_in_pie).
    EditorLevels->EditorRequestEndPlay();

    TSharedPtr<FJsonObject> StopRequested = MakeShared<FJsonObject>();
    StopRequested->SetBoolField(TEXT("pie_stopped"), true);
    StopRequested->SetBoolField(TEXT("was_in_pie"), bWasRunning);
    StopRequested->SetStringField(TEXT("message"), TEXT("Play-in-Editor stop requested. Use is_play_in_editor_active to verify."));
    StopRequested->SetBoolField(TEXT("still_in_pie"), EditorLevels->IsInPlayInEditor());
    return mcp_common::MakeOkReply(StopRequested);
}

// MCP:is_play_in_editor_active - THE oracle for "is the game running right now": asks ULevelEditorSubsystem::IsInPlayInEditor and returns is_pie_active plus status 'Playing'/'Stopped' and the current level name. Cheap, read-only, no params. Use it to confirm start_play_in_editor / stop_play_in_editor (both answer from before the async transition finishes) and as the gate before any save -- saving with PIE up is refused by the engine. WARNING: Do NOT use the pie_active field of list_dirty_packages for this: it can report false with PIE running and answering.
//   @returns { is_pie_active: bool, current_level: string (the EDITOR's current level, not the PIE world), status: 'Playing'|'Stopped' }

TSharedPtr<FJsonObject> HandleIsPlayInEditorActive(const TSharedPtr<FJsonObject>& Params)
{
    ULevelEditorSubsystem* EditorLevels = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
    if (EditorLevels == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Level Editor Subsystem not found"));
    }

    // The editor subsystem's own answer. current_level is the level being EDITED, not the PIE world.
    const bool bPlaying = EditorLevels->IsInPlayInEditor();
    const ULevel* EditedLevel = EditorLevels->GetCurrentLevel();

    TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
    State->SetBoolField(TEXT("is_pie_active"), bPlaying);
    State->SetStringField(TEXT("current_level"), EditedLevel != nullptr ? EditedLevel->GetName() : FString(TEXT("Unknown")));
    State->SetStringField(TEXT("status"), bPlaying ? TEXT("Playing") : TEXT("Stopped"));
    return mcp_common::MakeOkReply(State);
}

// MCP:wait_for_pie_completion - Block until the human closes Play-in-Editor (or the timeout fires), pumping the game thread while it polls, and report elapsed_seconds + whether it completed or timed out. Use it to hand the session to a human for a piloted run and get control back when they hit Stop; use is_play_in_editor_active for an instant, non-blocking check. Returns immediately with started_in_pie:false when PIE was not running.
//   @param timeout_seconds - Optional float (default 30). Upper bound on the wait; 0 or less means wait indefinitely.
//   @param poll_interval_seconds - Optional float (default 0.1). Seconds between IsInPlayInEditor probes; anything below 0.01 is floored to 0.01.
//   @returns { started_in_pie, still_active, timed_out, elapsed_seconds, completed }
//   @hints read-only
TSharedPtr<FJsonObject> HandleWaitForPieCompletion(const TSharedPtr<FJsonObject>& Params)
{
    ULevelEditorSubsystem* LevelEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
    if (!LevelEditorSubsystem) {
        return mcp_common::MakeErrorReply(TEXT("Level Editor Subsystem not found"));
    }
    const bool bStartedInPie = LevelEditorSubsystem->IsInPlayInEditor();
    if (!bStartedInPie) {
        TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
        R->SetBoolField(TEXT("started_in_pie"), false);
        R->SetBoolField(TEXT("still_active"), false);
        R->SetBoolField(TEXT("completed"), true);
        R->SetBoolField(TEXT("timed_out"), false);
        R->SetNumberField(TEXT("elapsed_seconds"), 0.0);
        R->SetStringField(TEXT("message"), TEXT("PIE was not active; nothing to wait for"));
        return mcp_common::MakeOkReply(R);
    }

    double TimeoutSec = 30.0;
    double PollSec = 0.1;
    double Tmp = 0.0;
    if (Params->TryGetNumberField(TEXT("timeout_seconds"), Tmp)) { TimeoutSec = FMath::Max(0.0, Tmp); }
    // Clamped to [0.01, 5] s -- FMath::Clamp takes the VALUE first, then Min and Max.
    if (Params->TryGetNumberField(TEXT("poll_interval_seconds"), Tmp)) { PollSec = FMath::Clamp(Tmp, 0.01, 5.0); }

    const double Start = FPlatformTime::Seconds();
    bool bTimedOut = false;
    while (LevelEditorSubsystem->IsInPlayInEditor())
    {
        const double Elapsed = FPlatformTime::Seconds() - Start;
        if (TimeoutSec > 0.0 && Elapsed > TimeoutSec) { bTimedOut = true; break; }
        const uint32 SleepMs = static_cast<uint32>(PollSec * 1000.0);
        FPlatformProcess::Sleep(SleepMs / 1000.0f);
        // Pump the FTSTicker / message loop a touch so async work doesn't stall.
        FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
    }
    const double ElapsedTotal = FPlatformTime::Seconds() - Start;
    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("started_in_pie"), true);
    R->SetBoolField(TEXT("still_active"), LevelEditorSubsystem->IsInPlayInEditor());
    R->SetBoolField(TEXT("completed"), !LevelEditorSubsystem->IsInPlayInEditor());
    R->SetBoolField(TEXT("timed_out"), bTimedOut);
    R->SetNumberField(TEXT("elapsed_seconds"), ElapsedTotal);
    return mcp_common::MakeOkReply(R);
}

// MCP:capture_pie_screenshot - Write a PNG of what the running game shows: the PIE game viewport, or the active editor viewport when there is no game viewport (with PIE not running, saved_to_pie_active=false and a note says so). It reads the pixels synchronously, so the file holds the 3D scene without the UMG/Slate HUD, and an existing file at output_path is overwritten. With include_ui=true during PIE it also queues an engine screenshot that a later frame crops to the game viewport, HUD included, and writes over the same file (ui_overwrite_next_frame=true): re-read the file after the call. Use it to SHOW a play-test result; for the editor viewport with a fresh redraw, resizing or base64 use capture_viewport, for a supersampled image capture_high_res_screenshot, and for actor state instead of pixels capture_pie_state. Start PIE with start_play_in_editor first. No viewport, a zero-size viewport or a failed write is an error. Changes nothing in the level; every call writes the file again.
//   @param output_path - File path of the PNG, absolute or relative to the editor's working directory, e.g. "D:/Shots/hud.png"; folders are created. Omit it to auto-name <Project>/Saved/MCP/PIE_<yyyyMMdd_HHmmss UTC>.png.
//   @param include_ui - true to also write the HUD version over output_path on a later frame (PIE with rendering only, SDR only, abandoned after 8 s, e.g. with an HDR editor display); default false keeps only the scene-only PNG.
//   @returns { saved_to_pie_active, output_path, width, height, bytes_written, includes_ui, ui_overwrite_next_frame, ui_note?, note? }
TSharedPtr<FJsonObject> HandleCapturePieScreenshot(const TSharedPtr<FJsonObject>& Params)
{
    ULevelEditorSubsystem* LevelEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
    if (!LevelEditorSubsystem) {
        return mcp_common::MakeErrorReply(TEXT("Level Editor Subsystem not found"));
    }
    // The with-UI path composites the game viewport + HUD (delegate + CPU crop; full
    // rationale at the OnScreenshotCaptured bind site below). Kept behind an explicit opt-in so
    // the default caller gets ONLY the crash-safe synchronous scene capture. Default false.
    bool bIncludeUi = false;
    Params->TryGetBoolField(TEXT("include_ui"), bIncludeUi);

    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("saved_to_pie_active"), LevelEditorSubsystem->IsInPlayInEditor());
    if (!LevelEditorSubsystem->IsInPlayInEditor()) {
        R->SetStringField(TEXT("note"), TEXT("PIE not active - capture is the editor viewport, not the play world"));
    }

    FViewport* Viewport = nullptr;
    if (GEngine && GEngine->GameViewport) { Viewport = GEngine->GameViewport->Viewport; }
    if (!Viewport && GEditor) { Viewport = GEditor->GetActiveViewport(); }
    if (!Viewport) {
        return mcp_common::MakeErrorReply(TEXT("No active viewport"));
    }

    const FIntPoint Size = Viewport->GetSizeXY();
    const int32 W = Size.X;
    const int32 H = Size.Y;
    if (W <= 0 || H <= 0) {
        return mcp_common::MakeErrorReply(TEXT("Viewport has zero size"));
    }

    TArray<FColor> Pixels;
    Pixels.SetNumZeroed(W * H);
    if (!Viewport->ReadPixels(Pixels, FReadSurfaceDataFlags())) {
        return mcp_common::MakeErrorReply(TEXT("ReadPixels failed"));
    }

    FString OutPath;
    Params->TryGetStringField(TEXT("output_path"), OutPath);
    if (OutPath.IsEmpty()) {
        const FString TimeStr = FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"));
        OutPath = FPaths::ProjectSavedDir() / TEXT("MCP") / FString::Printf(TEXT("PIE_%s.png"), *TimeStr);
    }
    // Absolute path so the synchronous PNG write and the deferred FScreenshotRequest below agree.
    OutPath = FPaths::ConvertRelativePathToFull(OutPath);
    // ImageWrapper encodes BGR8 -> PNG via the standard UE pipeline.
    IImageWrapperModule& IWM = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    TSharedPtr<IImageWrapper> Png = IWM.CreateImageWrapper(EImageFormat::PNG);
    if (!Png.IsValid()) {
        return mcp_common::MakeErrorReply(TEXT("PNG ImageWrapper unavailable"));
    }
    // ReadPixels can leave alpha at 0 -> a fully transparent PNG that renders
    // white/blank despite perfectly valid RGB. Force opaque before encoding,
    // exactly like capture_viewport does.
    for (FColor& Px : Pixels) { Px.A = 255; }
    Png->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), W, H, ERGBFormat::BGRA, 8);
    const TArray64<uint8>& PngBytesConst = Png->GetCompressed();
    TArray64<uint8> PngBytes(PngBytesConst);
    if (!FFileHelper::SaveArrayToFile(PngBytes, *OutPath)) {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to write PNG to %s"), *OutPath));
    }
    R->SetStringField(TEXT("output_path"), OutPath);
    R->SetNumberField(TEXT("width"), W);
    R->SetNumberField(TEXT("height"), H);
    R->SetNumberField(TEXT("bytes_written"), static_cast<int32>(PngBytes.Num()));

    // The ReadPixels above grabs only the scene render target — it never contains the
    // UMG/Slate HUD overlay. We can't composite a fresh frame synchronously here: this MCP
    // command runs on a blocked game thread, so no render/present happens until it returns.
    // So when PIE is active we ALSO queue a with-UI engine screenshot; a later render frame
    // hands us the composited whole-window bitmap via OnScreenshotCaptured(), which we CPU-crop
    // to the game viewport (editor chrome removed, HUD kept) and write over output_path.
    bool bUiOverwriteNextFrame = false;
    // Only arm the deferred HUD overwrite when the caller explicitly opts in AND
    // rendering is available (skip under -nullrhi) AND a game viewport exists. This keeps the
    // scene-only PNG above byte-identical for every default caller.
    if (bIncludeUi && LevelEditorSubsystem->IsInPlayInEditor() && FApp::CanEverRender() && GEngine && GEngine->GameViewport) {
        // Delegate + CPU crop, so the file holds the game viewport + HUD ONLY. bInRestrictToGameViewport
        // stays false (the AV-free whole-window Slate path -- the restrict=true AV lives INSIDE
        // TakeScreenshot(SViewport), which binding a delegate does NOT bypass), and that path alone
        // writes the WHOLE editor window (chrome + HUD). So we bind a ONE-SHOT handler on the static
        // UGameViewportClient::OnScreenshotCaptured(). That delegate fires on the GAME THREAD from
        // ProcessScreenShots with the composited whole-window FColor bitmap (HUD included) and is called
        // INSTEAD of the engine writing the file when bound -- so our lambda MUST write it. We CPU-crop
        // the bitmap to the game-viewport sub-rect (Slate widget geometry), force opaque alpha, PNG-encode
        // to output_path, then Remove the handle. A *Fired guard + immediate Remove + an 8s FTSTicker
        // safety-unregister protect the process-wide delegate from hijack/dangling (e.g. an HDR editor
        // swap-chain -- GetSceneHDREnabled() -- routes to a DIFFERENT HDR delegate, so this SDR lambda
        // never fires -> the ticker cleans up and the scene-only PNG from the synchronous ReadPixels above
        // is preserved). SDR only: the HDR delegate is intentionally NOT bound.
        // The default path (include_ui=false, synchronous scene ReadPixels) never gets here and is crash-safe.
        TSharedRef<bool> Fired = MakeShared<bool>(false);
        TSharedRef<FDelegateHandle> Handle = MakeShared<FDelegateHandle>();
        const FString CaptureOutPath = OutPath; // already ConvertRelativePathToFull

        auto EncodeAndWrite = [CaptureOutPath](int32 InW, int32 InH, const TArray<FColor>& Full) -> bool {
            int32 CropX = 0, CropY = 0, CropW = InW, CropH = InH;
            bool bCropped = false;
            UGameViewportClient* GVC = GEngine ? GEngine->GameViewport : nullptr;
            TSharedPtr<SViewport> VpWidget = GVC ? GVC->GetGameViewportWidget() : nullptr;
            TSharedPtr<SWindow> Win = GVC ? GVC->GetWindow() : nullptr;
            if (VpWidget.IsValid() && Win.IsValid() && FSlateApplication::IsInitialized())
            {
                FWidgetPath WP;
                // NOTE: GeneratePathToWidget does NOT exist -- use the Unchecked (bool) variant.
                if (FSlateApplication::Get().GeneratePathToWidgetUnchecked(VpWidget.ToSharedRef(), WP))
                {
                    FArrangedWidget AW = WP.FindArrangedWidget(VpWidget.ToSharedRef()).Get(FArrangedWidget::GetNullWidget());
                    const FVector2f Pos = FVector2f(AW.Geometry.AbsolutePosition);
                    const FVector2f Sz  = FVector2f(AW.Geometry.GetDrawSize());
                    const FVector2f WinPos = FVector2f(Win->GetPositionInScreen());
                    CropX = FMath::Clamp((int32)(Pos.X - WinPos.X), 0, InW);
                    CropY = FMath::Clamp((int32)(Pos.Y - WinPos.Y), 0, InH);
                    CropW = FMath::Clamp((int32)Sz.X, 0, InW - CropX);
                    CropH = FMath::Clamp((int32)Sz.Y, 0, InH - CropY);
                    bCropped = (CropW > 0 && CropH > 0 && (CropW < InW || CropH < InH));
                }
            }
            if (CropW <= 0 || CropH <= 0) { CropX = 0; CropY = 0; CropW = InW; CropH = InH; }
            if (!bCropped)
            {
                // Load-bearing: if the game-viewport widget is null/degenerate we fall back to the
                // whole window. Log it so a smoke test can tell "viewport-only" from "full window".
                UE_LOG(LogTemp, Warning, TEXT("[MCP] capture_pie_screenshot include_ui: could not crop to game viewport; wrote FULL window (%dx%d). GetGameViewportWidget null or degenerate rect."), InW, InH);
            }
            TArray<FColor> Crop; Crop.SetNumUninitialized(CropW * CropH);
            for (int32 y = 0; y < CropH; ++y)
                FMemory::Memcpy(&Crop[y * CropW], &Full[(CropY + y) * InW + CropX], CropW * sizeof(FColor));
            for (FColor& Px : Crop) { Px.A = 255; }
            IImageWrapperModule& IWM = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
            TSharedPtr<IImageWrapper> Png = IWM.CreateImageWrapper(EImageFormat::PNG);
            if (!Png.IsValid()) { return false; }
            Png->SetRaw(Crop.GetData(), Crop.Num() * sizeof(FColor), CropW, CropH, ERGBFormat::BGRA, 8);
            const TArray64<uint8>& C = Png->GetCompressed(); TArray64<uint8> Bytes(C);
            return FFileHelper::SaveArrayToFile(Bytes, *CaptureOutPath);
        };

        *Handle = UGameViewportClient::OnScreenshotCaptured().AddLambda(
            [Fired, Handle, EncodeAndWrite](int32 W2, int32 H2, const TArray<FColor>& Bmp){
                if (*Fired) return; *Fired = true;
                EncodeAndWrite(W2, H2, Bmp);
                UGameViewportClient::OnScreenshotCaptured().Remove(*Handle);
            });

        // Safety unregister (e.g. HDR swap-chain routes to the HDR delegate, or no frame renders).
        FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
            [Fired, Handle](float) -> bool {
                if (!*Fired) { *Fired = true; UGameViewportClient::OnScreenshotCaptured().Remove(*Handle); }
                return false;
            }), 8.0f);

#if UE_VERSION_OLDER_THAN(5, 8, 0)
        // 5.7 has no bInRestrictToGameViewport: it always captures the whole window, which is what false asks 5.8 for.
        FScreenshotRequest::RequestScreenshot(OutPath, /*bInShowUI=*/true, /*bAddFilenameSuffix=*/false,
            /*bHdrScreenshot=*/false, FIntRect());
#else
        FScreenshotRequest::RequestScreenshot(OutPath, /*bInShowUI=*/true, /*bAddFilenameSuffix=*/false,
            /*bHdrScreenshot=*/false, FIntRect(), /*bInRestrictToGameViewport=*/false);
#endif
        bUiOverwriteNextFrame = true;
    }
    R->SetBoolField(TEXT("includes_ui"), false);
    R->SetBoolField(TEXT("ui_overwrite_next_frame"), bUiOverwriteNextFrame);
    if (bUiOverwriteNextFrame) {
        R->SetStringField(TEXT("ui_note"), TEXT("A game-viewport-only version (cropped, HUD composited, editor chrome removed) overwrites output_path on a later render frame; re-read the file after this call to get the HUD."));
    }
    return mcp_common::MakeOkReply(R);
}

// ============================================================================
// Synthetic input injection + pawn possession into an active PIE session.
// These commands share the same prerequisite: a real PlayInEditor session (spawned by
// start_play_in_editor, which requests EPlaySessionWorldType::PlayInEditor, not
// SimulateInEditor) and its PlayerController, resolved via GEditor->GetPIEWorldContext
// (UNREALED_API) -> UWorld::GetFirstPlayerController(). Engine source:
//   FInputKeyEventArgs::CreateSimulated  Runtime/Engine/Public/InputKeyEventArgs.h
//   APlayerController::InputKey          Runtime/Engine/Classes/GameFramework/PlayerController.h
//   AController::Possess/UnPossess       Runtime/Engine/Classes/GameFramework/Controller.h
// ============================================================================

namespace
{
    // Resolve the PlayerController of the Nth PIE client (0 = first/server-or-standalone).
    // Returns nullptr with an honest OutError if PIE isn't active or the index is out of range.
    APlayerController* ResolvePieClientController(int32 PieClientIndex, FString& OutError)
    {
        if (!GEditor)
        {
            OutError = TEXT("No editor available");
            return nullptr;
        }
        FWorldContext* WC = GEditor->GetPIEWorldContext(PieClientIndex);
        if (!WC || !WC->World())
        {
            OutError = FString::Printf(TEXT("No active PIE session for pie_client_index %d"), PieClientIndex);
            return nullptr;
        }
        APlayerController* PC = WC->World()->GetFirstPlayerController();
        if (!PC)
        {
            OutError = FString::Printf(TEXT("PIE client %d has no PlayerController (world may still be loading)"), PieClientIndex);
            return nullptr;
        }
        return PC;
    }

    // Inject one simulated key, mouse-button or axis event into a PlayerController. Temporarily
    // disables bFilterInputByPlatformUser (save/restore) so INPUTDEVICEID_NONE's simulated
    // event isn't dropped for not mapping to the target PlayerController's exact platform user
    // -- a gotcha of UInputSettings/IPlatformInputDeviceMapper.
    void InjectSimulatedKeyEvent(APlayerController* PC, const FKey& Key, EInputEvent Event, float AmountDepressed)
    {
        UInputSettings* InputSettings = UInputSettings::GetInputSettings();
        const bool bPrevFilter = InputSettings ? InputSettings->bFilterInputByPlatformUser : false;
        if (InputSettings) { InputSettings->bFilterInputByPlatformUser = false; }

        const FInputKeyEventArgs Args = FInputKeyEventArgs::CreateSimulated(Key, Event, AmountDepressed);
        PC->InputKey(Args);

        if (InputSettings) { InputSettings->bFilterInputByPlatformUser = bPrevFilter; }
    }

    bool ParseDigitalInputEvent(const FString& EventStr, EInputEvent& OutEvent)
    {
        if (EventStr.Equals(TEXT("Pressed"), ESearchCase::IgnoreCase)) { OutEvent = IE_Pressed; return true; }
        if (EventStr.Equals(TEXT("Released"), ESearchCase::IgnoreCase)) { OutEvent = IE_Released; return true; }
        if (EventStr.Equals(TEXT("Repeat"), ESearchCase::IgnoreCase)) { OutEvent = IE_Repeat; return true; }
        return false;
    }
}

// MCP:inject_key_input - Press or release a keyboard/gamepad key at a running PIE session's PlayerController, so an agent can drive the game headless -- jump, interact, open a menu. Press and release are SEPARATE calls: a tap is two of them, a hold is press now and release later. Requires an active PIE session; use inject_analog_input for sticks and triggers (with its hold_seconds for sustained movement) and inject_mouse_button for clicks. WARNING: `injected` only means the event reached the controller: whether the game DID anything is a state read, and in co-op read the right world.
//   @param key - Required key name as Unreal spells it, e.g. "SpaceBar", "W", "Gamepad_FaceButton_Bottom". An unknown name is a clean error, never a silent no-op.
//   @param event - Optional "Pressed" (default), "Released" or "Repeat". Remember the matching Released, or the game keeps thinking the key is down.
//   @param pie_client_index - Optional int (default 0). Which PIE client window to target -- 0 is the server/host window in a ListenServer session, 1 the first client.
//   @param amount_depressed - Optional float (default 1.0). Only meaningful on the controller route.
//   @param route - Optional "controller" (default), "slate" or "both". NOTE: THIS IS THE DIFFERENCE BETWEEN DRIVING THE GAME AND DRIVING THE UI: the controller route goes through PlayerController::InputKey, which is the gameplay input layer -- it moves the character and fires Enhanced Input actions but NEVER moves UI focus. The slate route goes through FSlateApplication::ProcessKeyDownEvent, which is the only one that moves FOCUS, so it is the only way to test a menu's gamepad navigation headless. They are not interchangeable; "both" sends to each.
//   @returns { injected: bool, key: string, event: string, pie_client_index: int, route: string, controller_injected: bool, slate_injected: bool, slate_handled: bool, slate_note? } -- NOTE: slate_handled is Slate's own verdict on whether anything consumed the key: false usually means nothing had focus, which is the useful signal (not a failure of the call).
TSharedPtr<FJsonObject> HandleInjectKeyInput(const TSharedPtr<FJsonObject>& Params)
{
    FString KeyName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("key"), KeyName)) { return Missing; }
    const FKey Key(*KeyName);
    if (!Key.IsValid())
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown key: %s"), *KeyName));
    }

    FString EventStr = TEXT("Pressed");
    Params->TryGetStringField(TEXT("event"), EventStr);
    EInputEvent Event;
    if (!ParseDigitalInputEvent(EventStr, Event))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown event '%s' (expected Pressed|Released|Repeat)"), *EventStr));
    }

    const int32 PieClientIndex = UEAgentLevelCommandsImpl::ReadPieClientIndex(Params);
    double AmountDepressed = 1.0;
    Params->TryGetNumberField(TEXT("amount_depressed"), AmountDepressed);

    // There are TWO possible destinations and they are not interchangeable.
    //   "controller" -> PC->InputKey: the GAMEPLAY input layer. Moves the character, fires
    //                   Enhanced Input actions. Does NOT move UI focus.
    //   "slate"      -> FSlateApplication::ProcessKeyDownEvent: the UI layer. The only one that
    //                   moves FOCUS, i.e. the only one that can test a menu's gamepad
    //                   navigation. Does NOT reach gameplay.
    // The default is "controller": changing it would silently break every existing call.
    FString Route = TEXT("controller");
    Params->TryGetStringField(TEXT("route"), Route);
    const FString RouteLower = Route.ToLower();
    const bool bRouteController = (RouteLower == TEXT("controller") || RouteLower == TEXT("both"));
    const bool bRouteSlate      = (RouteLower == TEXT("slate")      || RouteLower == TEXT("both"));
    if (!bRouteController && !bRouteSlate)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Unknown route '%s' (expected controller|slate|both)"), *Route));
    }

    bool bControllerInjected = false;
    if (bRouteController)
    {
        FString ResolveError;
        APlayerController* PC = ResolvePieClientController(PieClientIndex, ResolveError);
        if (!PC)
        {
            return mcp_common::MakeErrorReply(ResolveError);
        }
        InjectSimulatedKeyEvent(PC, Key, Event, static_cast<float>(AmountDepressed));
        bControllerInjected = true;
    }

    bool bSlateInjected = false;
    bool bSlateHandled = false;
    FString SlateNote;
    if (bRouteSlate)
    {
        if (!FSlateApplication::IsInitialized())
        {
            SlateNote = TEXT("Slate is not initialized in this process: the slate route did nothing.");
        }
        else
        {
            // CharacterCode/KeyCode = 0: this is a synthetic key, not a character. The engine does
            // the same for its own non-character events (Events.h).
            const FKeyEvent KeyEventArgs(Key, FModifierKeysState(), /*UserIndex=*/ 0,
                                         /*bIsRepeat=*/ Event == IE_Repeat, /*CharacterCode=*/ 0, /*KeyCode=*/ 0);
            if (Event == IE_Released)
            {
                bSlateHandled = FSlateApplication::Get().ProcessKeyUpEvent(KeyEventArgs);
            }
            else
            {
                bSlateHandled = FSlateApplication::Get().ProcessKeyDownEvent(KeyEventArgs);
            }
            bSlateInjected = true;
        }
    }

    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("injected"), bControllerInjected || bSlateInjected);
    R->SetStringField(TEXT("key"), KeyName);
    R->SetStringField(TEXT("event"), EventStr);
    R->SetNumberField(TEXT("pie_client_index"), PieClientIndex);
    R->SetStringField(TEXT("route"), RouteLower);
    R->SetBoolField(TEXT("controller_injected"), bControllerInjected);
    R->SetBoolField(TEXT("slate_injected"), bSlateInjected);
    // slate_handled is Slate's verdict: false means NOBODY consumed the key -- usually that
    // nothing had focus. That is the useful signal, not a failure of the call.
    R->SetBoolField(TEXT("slate_handled"), bSlateHandled);
    if (!SlateNote.IsEmpty())
    {
        R->SetStringField(TEXT("slate_note"), SlateNote);
    }
    return mcp_common::MakeOkReply(R);
}

// MCP:inject_analog_input - Feed an analog axis value (gamepad stick, trigger) to a running PIE session's PlayerController -- how you actually MOVE or LOOK headless, where inject_key_input only does digital keys. NOTE: A single axis sample decays to zero within a tick, so sustained motion needs hold_seconds, which re-injects every tick for that long and then neutralises with a 0.0; it is fire-and-forget and returns immediately. Requires an active PIE session and a genuinely analog key (a digital one is a clean error).
//   @param key - Required analog key name, e.g. "Gamepad_LeftX", "Gamepad_LeftTriggerAxis". Rejected with a clean error if the key is not analog.
//   @param value - Required float axis value; roughly -1..1 for sticks, 0..1 for triggers.
//   @param pie_client_index - Optional int (default 0). Which PIE client window to target -- 0 is the server/host window in a ListenServer session, 1 the first client.
//   @param hold_seconds - Optional float. When > 0, the analog value is re-injected every tick for this many seconds (a single axis sample decays to zero, so sustained motion needs re-feeding), then a final 0.0 is injected to neutralize. Fire-and-forget: returns immediately and the hold runs across the next hold_seconds of PIE. Omit (or 0) for a single sample.
//   @returns { injected: bool, key: string, value: number, pie_client_index: int, hold_seconds: number }
TSharedPtr<FJsonObject> HandleInjectAnalogInput(const TSharedPtr<FJsonObject>& Params)
{
    FString KeyName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("key"), KeyName)) { return Missing; }
    const FKey Key(*KeyName);
    if (!Key.IsValid())
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown key: %s"), *KeyName));
    }
    if (!Key.IsAnalog())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Key '%s' is not analog (use inject_key_input for digital keys)"), *KeyName));
    }

    double Value = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("value"), Value)) { return Missing; }

    const int32 PieClientIndex = UEAgentLevelCommandsImpl::ReadPieClientIndex(Params);

    FString ResolveError;
    APlayerController* PC = ResolvePieClientController(PieClientIndex, ResolveError);
    if (!PC)
    {
        return mcp_common::MakeErrorReply(ResolveError);
    }

    InjectSimulatedKeyEvent(PC, Key, IE_Axis, static_cast<float>(Value));

    // Optional sustained hold: an axis sample is consumed and decays, so re-feed the same value every
    // tick for hold_seconds, then inject 0.0 to neutralize. Weak-guarded so a mid-hold PIE stop just
    // unregisters the ticker.
    double HoldSeconds = 0.0;
    Params->TryGetNumberField(TEXT("hold_seconds"), HoldSeconds);
    if (HoldSeconds > 0.0)
    {
        TWeakObjectPtr<APlayerController> WeakPC(PC);
        const FKey HeldKey = Key;
        const float HeldValue = static_cast<float>(Value);
        const double TotalHold = HoldSeconds;
        TSharedRef<double> Elapsed = MakeShared<double>(0.0);
        FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
            [WeakPC, HeldKey, HeldValue, TotalHold, Elapsed](float DeltaTime) -> bool
            {
                APlayerController* PCNow = WeakPC.Get();
                if (!PCNow || !PCNow->GetWorld())
                {
                    return false; // PIE gone -> unregister
                }
                *Elapsed += DeltaTime;
                const bool bDone = (*Elapsed >= TotalHold);
                const float SampleVal = bDone ? 0.0f : HeldValue;
                InjectSimulatedKeyEvent(PCNow, HeldKey, IE_Axis, SampleVal);
                return !bDone; // stop after the final neutralizing sample
            }), 0.0f);
    }

    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("injected"), true);
    R->SetStringField(TEXT("key"), KeyName);
    R->SetNumberField(TEXT("value"), Value);
    R->SetNumberField(TEXT("pie_client_index"), PieClientIndex);
    R->SetNumberField(TEXT("hold_seconds"), HoldSeconds);
    return mcp_common::MakeOkReply(R);
}

// MCP:inject_mouse_button - Fire a simulated mouse click at a running PIE session's PlayerController: fire/aim/interact without a human at the mouse. Press and release are SEPARATE calls, so a click is two of them and a hold is press now, release later. It injects the button only -- it does not move the cursor, so aim with set_control_rotation first. Requires an active PIE session; use inject_key_input for keyboard/gamepad buttons and click_pie_widget for UMG buttons, which this does NOT reach.
//   @param button - Optional "Left"|"Right"|"Middle" (default "Left"); anything else is a clean error.
//   @param event - Optional "Pressed" (default) or "Released". A full click needs both, in two calls.
//   @param pie_client_index - Optional int (default 0). Which PIE client window to aim at -- 0 is the server/host window in a ListenServer session, 1 the first client.
//   @returns { injected: bool (the event was delivered to the controller, NOT that the game reacted -- verify with state), button: string, event: string, pie_client_index: int }
TSharedPtr<FJsonObject> HandleInjectMouseButton(const TSharedPtr<FJsonObject>& Params)
{
    FString ButtonStr = TEXT("Left");
    Params->TryGetStringField(TEXT("button"), ButtonStr);
    FKey Key;
    if (ButtonStr.Equals(TEXT("Left"), ESearchCase::IgnoreCase)) { Key = EKeys::LeftMouseButton; }
    else if (ButtonStr.Equals(TEXT("Right"), ESearchCase::IgnoreCase)) { Key = EKeys::RightMouseButton; }
    else if (ButtonStr.Equals(TEXT("Middle"), ESearchCase::IgnoreCase)) { Key = EKeys::MiddleMouseButton; }
    else
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown button '%s' (expected Left|Right|Middle)"), *ButtonStr));
    }

    FString EventStr = TEXT("Pressed");
    Params->TryGetStringField(TEXT("event"), EventStr);
    EInputEvent Event;
    if (!EventStr.Equals(TEXT("Pressed"), ESearchCase::IgnoreCase) && !EventStr.Equals(TEXT("Released"), ESearchCase::IgnoreCase))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown event '%s' (expected Pressed|Released)"), *EventStr));
    }
    Event = EventStr.Equals(TEXT("Pressed"), ESearchCase::IgnoreCase) ? IE_Pressed : IE_Released;

    const int32 PieClientIndex = UEAgentLevelCommandsImpl::ReadPieClientIndex(Params);

    FString ResolveError;
    APlayerController* PC = ResolvePieClientController(PieClientIndex, ResolveError);
    if (!PC)
    {
        return mcp_common::MakeErrorReply(ResolveError);
    }

    InjectSimulatedKeyEvent(PC, Key, Event, 1.0f);

    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("injected"), true);
    R->SetStringField(TEXT("button"), ButtonStr);
    R->SetStringField(TEXT("event"), EventStr);
    R->SetNumberField(TEXT("pie_client_index"), PieClientIndex);
    return mcp_common::MakeOkReply(R);
}

// MCP:possess_pawn - Make the first PlayerController of a running PIE session possess a pawn of that PIE world (AController::Possess), so input and camera go to it; the pawn it controlled before is unpossessed and stays in the world. Requires PIE: start it with start_play_in_editor; with no PIE session for pie_client_index, or a name that is not a pawn in that world, it is an error and nothing changes. Then drive it with inject_key_input or inject_analog_input, or aim it with set_control_rotation. Runtime only: nothing is saved and there is no undo. A pawn the controller already has is a success with already_possessed=true and no call. The engine's authority gate silently skips the possession on a pure client's PlayerController (Standalone and ListenServer have authority): then possessed=false, had_authority=false and a note, still a success, so always read possessed.
//   @param pawn_actor_name - Name or label of the pawn actor in the PIE world, e.g. "BP_Vehicle_C_0" or "Truck"; find_actors_by_name or get_actors_in_level with world="pie" list them.
//   @param pie_client_index - Which PIE instance, a whole number from 0 (default 0: the first, the server in a ListenServer session); 1, 2... for further clients.
//   @returns { possessed: bool, already_possessed: bool, had_authority: bool, pawn_actor_name: string, pie_client_index: int, note?: string }
TSharedPtr<FJsonObject> HandlePossessPawn(const TSharedPtr<FJsonObject>& Params)
{
    FString PawnActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pawn_actor_name"), PawnActorName)) { return Missing; }
    const int32 PieClientIndex = UEAgentLevelCommandsImpl::ReadPieClientIndex(Params);

    FString ResolveError;
    APlayerController* PC = ResolvePieClientController(PieClientIndex, ResolveError);
    if (!PC)
    {
        return mcp_common::MakeErrorReply(ResolveError);
    }

    AActor* FoundActor = mcp_common::FindActorByName(PC->GetWorld(), PawnActorName);
    APawn* FoundPawn = Cast<APawn>(FoundActor);
    if (!FoundPawn)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Pawn actor not found in the PIE world: %s"), *PawnActorName));
    }

    const bool bAlreadyPossessed = (PC->GetPawn() == FoundPawn);
    const bool bHadAuthority = PC->HasAuthority();
    if (!bAlreadyPossessed)
    {
        PC->Possess(FoundPawn);
    }

    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    const bool bPossessedNow = (PC->GetPawn() == FoundPawn);
    R->SetBoolField(TEXT("possessed"), bPossessedNow);
    R->SetBoolField(TEXT("already_possessed"), bAlreadyPossessed);
    R->SetBoolField(TEXT("had_authority"), bHadAuthority);
    R->SetStringField(TEXT("pawn_actor_name"), PawnActorName);
    R->SetNumberField(TEXT("pie_client_index"), PieClientIndex);
    if (!bPossessedNow && !bHadAuthority)
    {
        R->SetStringField(TEXT("note"), TEXT("Possess() is a silent no-op without authority (engine gate); this PlayerController is not authoritative (likely a pure Client net_mode)"));
    }
    return mcp_common::MakeOkReply(R);
}

// Posing the PIE view without a bespoke camera actor: aim the control rotation and/or make any actor
// the PlayerController's view target.

// MCP:set_control_rotation - Set a Controller's control rotation (AController::SetControlRotation) in the active PIE session -- aims the view and, for a possessed pawn, its control-driven facing. Targets the named pawn's controller, a named controller, or (no target_name) the PIE session's PlayerController. PIE-gated: clean error if no PIE session.
//   @param pitch - Required number. Control rotation pitch (degrees).
//   @param yaw - Required number. Control rotation yaw (degrees).
//   @param roll - Required number. Control rotation roll (degrees).
//   @param target_name - Optional. Name/label of the possessed pawn (uses its controller) or a controller; default = the PIE session's PlayerController.
//   @param pie_client_index - Optional int (default 0).
//   @returns { applied: bool, target: string, control_rotation: [pitch, yaw, roll], pie_client_index: int }
TSharedPtr<FJsonObject> HandleSetControlRotation(const TSharedPtr<FJsonObject>& Params)
{
    double PitchD = 0.0, YawD = 0.0, RollD = 0.0;
    if (!Params->TryGetNumberField(TEXT("pitch"), PitchD) ||
        !Params->TryGetNumberField(TEXT("yaw"), YawD) ||
        !Params->TryGetNumberField(TEXT("roll"), RollD))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter(s): pitch, yaw, roll (numbers)"));
    }
    const FRotator NewRot(static_cast<float>(PitchD), static_cast<float>(YawD), static_cast<float>(RollD));
    if (NewRot.ContainsNaN())
    {
        return mcp_common::MakeErrorReply(TEXT("control rotation contains NaN (pitch/yaw/roll must be finite)"));
    }
    const int32 PieClientIndex = UEAgentLevelCommandsImpl::ReadPieClientIndex(Params);

    FString ResolveError;
    APlayerController* PC = ResolvePieClientController(PieClientIndex, ResolveError);
    if (!PC)
    {
        return mcp_common::MakeErrorReply(ResolveError);
    }

    // Resolve the target controller: an explicit pawn/controller by name, or the
    // PIE session's PlayerController when no target_name is given.
    AController* TargetController = PC;
    FString TargetName;
    if (Params->TryGetStringField(TEXT("target_name"), TargetName) && !TargetName.IsEmpty())
    {
        AActor* FoundActor = mcp_common::FindActorByName(PC->GetWorld(), TargetName);
        if (APawn* AsPawn = Cast<APawn>(FoundActor))
        {
            TargetController = AsPawn->GetController();
        }
        else if (AController* AsController = Cast<AController>(FoundActor))
        {
            TargetController = AsController;
        }
        else
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("target_name is neither a Pawn nor a Controller in the PIE world: %s"), *TargetName));
        }
        if (!TargetController)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Pawn '%s' has no controller to set control rotation on"), *TargetName));
        }
    }

    TargetController->SetControlRotation(NewRot);
    const FRotator Effective = TargetController->GetControlRotation();

    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("applied"), true);
    R->SetStringField(TEXT("target"), TargetController->GetName());
    R->SetArrayField(TEXT("control_rotation"), mcp_common::RotatorToJsonArray(Effective));
    R->SetNumberField(TEXT("pie_client_index"), PieClientIndex);
    return mcp_common::MakeOkReply(R);
}

// MCP:set_view_target - Point the active PIE session's PlayerController camera at an actor (APlayerController::SetViewTargetWithBlend). blend_time=0 cuts instantly; >0 blends linearly. PIE-gated: clean error if no PIE session. Frames captures without spawning a camera by hand.
//   @param target_actor_name - Required. Name/label of the actor to view through (its camera/eyes) in the PIE world.
//   @param blend_time - Optional number (default 0). Seconds to blend from the current view target.
//   @param pie_client_index - Optional int (default 0).
//   @returns { applied: bool, view_target: string, blend_time: float, pie_client_index: int, note?: string }
TSharedPtr<FJsonObject> HandleSetViewTarget(const TSharedPtr<FJsonObject>& Params)
{
    FString TargetActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target_actor_name"), TargetActorName)) { return Missing; }
    double BlendTimeD = 0.0;
    Params->TryGetNumberField(TEXT("blend_time"), BlendTimeD);
    const int32 PieClientIndex = UEAgentLevelCommandsImpl::ReadPieClientIndex(Params);

    FString ResolveError;
    APlayerController* PC = ResolvePieClientController(PieClientIndex, ResolveError);
    if (!PC)
    {
        return mcp_common::MakeErrorReply(ResolveError);
    }

    AActor* Target = mcp_common::FindActorByName(PC->GetWorld(), TargetActorName);
    if (!Target)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("View-target actor not found in the PIE world: %s"), *TargetActorName));
    }

    // Canonical single route (SetViewTargetWithBlend subsumes SetViewTarget and
    // exposes explicit Linear/0 defaults, avoiding FViewTargetTransitionParams'
    // non-neutral Cubic default). blend_time=0 => instant cut.
    PC->SetViewTargetWithBlend(Target, static_cast<float>(BlendTimeD));
    AActor* Effective = PC->GetViewTarget();

    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    const bool bApplied = (Effective == Target);
    R->SetBoolField(TEXT("applied"), bApplied);
    R->SetStringField(TEXT("view_target"), Effective ? Effective->GetName() : FString());
    R->SetNumberField(TEXT("blend_time"), BlendTimeD);
    R->SetNumberField(TEXT("pie_client_index"), PieClientIndex);
    if (!bApplied && BlendTimeD > 0.0)
    {
        R->SetStringField(TEXT("note"), TEXT("Blend in progress; view_target reflects the current (outgoing) target for this frame while the camera transitions."));
    }
    return mcp_common::MakeOkReply(R);
}

// ============================================================================
// World Settings Tuning Commands
// Operate on AWorldSettings of the editor world. Engine source:
//   Engine/Source/Runtime/Engine/Classes/GameFramework/WorldSettings.h
//   - float GlobalGravityZ;                (public, line ~580)
//   - uint8 bGlobalGravitySet:1;           (public, "Override World Gravity", line ~419)
//   - float KillZ;                         (public, line ~564)
//   - TSubclassOf<UDamageType> KillZDamageType; (public, line ~568)
//   - uint8 bEnableWorldBoundsChecks:1;    (public, line ~379)
//   - float WorldToMeters;                 (public, line ~560)
//   - ENGINE_API virtual float GetGravityZ() const;  (public, line ~819)
//   - TSubclassOf<AGameModeBase> DefaultGameMode;    (public, line ~599)
// All accessed fields are public members of AWorldSettings (the class is
// MinimalAPI, but only function-export needs the macro; public data members are
// accessible directly). GetGravityZ() is ENGINE_API-exported.
// ============================================================================

// Helper: resolve the editor world's AWorldSettings.
static AWorldSettings* GetEditorWorldSettings(FString& OutError)
{
    UWorld* World = mcp_common::GetEditorWorld();
    if (!World || !World->IsValidLowLevel())
    {
        OutError = TEXT("No valid editor world found");
        return nullptr;
    }
    AWorldSettings* WorldSettings = World->GetWorldSettings();
    if (!WorldSettings)
    {
        OutError = TEXT("WorldSettings not found");
        return nullptr;
    }
    return WorldSettings;
}

// MCP:set_world_gravity - Set an exact global gravity on the EDITOR world's WorldSettings, or one from a named preset (Earth -980, Moon -163, Mars -371, Zero, Low -490, High -1960, Enabled -980, Disabled 0): writes GlobalGravityZ and the "Override World Gravity" flag, then RE-READS GetGravityZ() into the reply so gravity_z is a measurement rather than an echo, next to the previous values and a census of the world's physics components with an impact rating. Earth is about -980 with the default centimetre unit. Undoable (wrapped in a transaction) but it dirties the MAP -- save_level to keep it, and check the .umap is not read-only or the save writes nothing. Read it back with get_world_settings.
//   @param gravity_z - Custom gravity along Z in cm/s^2 (negative = downward; Earth is about -980). Required unless preset is given; sending both is refused.
//   @param preset - Optional preset NAME instead of gravity_z, in any capitalisation: 'Earth' (-980), 'Moon' (-163), 'Mars' (-371), 'Zero' (0), 'Low' (-490), 'High' (-1960), 'Enabled' (-980), 'Disabled' (an override of 0). An unknown name is refused and nothing is written.
//   @param enable_override - Optional bool (default true). True makes the world use the gravity; false disables the override so the project default applies again.
//   @returns { applied_gravity_z: number (what landed in GlobalGravityZ), enable_override: bool, gravity_z: number (re-read GetGravityZ, the effective value), previous_gravity_z: number (effective gravity before the write), previous_override: bool (Override World Gravity before the write), preset?: string (echo), world_name: string, world_physics_component_count: int (actors with a primitive component), world_simulate_physics_component_count: int, world_dynamic_physics_component_count: int (Movable), gravity_impact: number, gravity_impact_rating: string, gravity_recommendations: [string], success: bool (hardcoded) }

TSharedPtr<FJsonObject> HandleSetWorldGravity(const TSharedPtr<FJsonObject>& Params)
{
    // gravity_z or a named preset: exactly one of them.
    double GravityZ = 0.0;
    const bool bHasGravityZ = Params->TryGetNumberField(TEXT("gravity_z"), GravityZ);
    FString PresetName;
    const bool bHasPreset = Params->TryGetStringField(TEXT("preset"), PresetName);
    bool bEnableOverride = true;
    Params->TryGetBoolField(TEXT("enable_override"), bEnableOverride);
    if (bHasGravityZ == bHasPreset)
    {
        return mcp_common::MakeErrorReply(bHasPreset
            ? TEXT("set_world_gravity: send gravity_z OR preset, not both; nothing was written")
            : TEXT("Missing required parameter 'gravity_z' (or send 'preset' with a preset name)"));
    }
    if (bHasPreset)
    {
        const UEAgentLevelCommandsImpl::FGravityPreset* Preset = UEAgentLevelCommandsImpl::FindGravityPreset(PresetName);
        if (Preset == nullptr)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("set_world_gravity: unknown preset '%s'; use Earth, Moon, Mars, Zero, Low, High, Enabled or Disabled. Nothing was written."),
                *PresetName));
        }
        GravityZ = Preset->GravityZ;
    }

    FString Error;
    AWorldSettings* WorldSettings = GetEditorWorldSettings(Error);
    if (!WorldSettings)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    UWorld* World = WorldSettings->GetWorld();

    // State before the write.
    const float PreviousGravity = WorldSettings->GetGravityZ();
    const bool bPreviousOverride = WorldSettings->bGlobalGravitySet != 0;

    FScopedTransaction Transaction(FText::FromString(TEXT("Set World Gravity")));
    WorldSettings->Modify();

    WorldSettings->bGlobalGravitySet = bEnableOverride;
    WorldSettings->GlobalGravityZ = static_cast<float>(GravityZ);
    // GetGravityZ() caches WorldGravityZ via bWorldGravitySet; reset that transient
    // flag so the next GetGravityZ() recomputes from the (possibly) new override.
    WorldSettings->bWorldGravitySet = false;

    const float ReadBackGravity = WorldSettings->GetGravityZ();

    // Physics census for context: the first primitive component of every actor.
    int32 PrimitiveCount = 0;
    int32 SimulatingCount = 0;
    int32 MovableCount = 0;
    if (World != nullptr)
    {
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            const UPrimitiveComponent* FirstPrimitive = It->FindComponentByClass<UPrimitiveComponent>();
            if (FirstPrimitive == nullptr)
            {
                continue;
            }
            ++PrimitiveCount;
            if (FirstPrimitive->IsSimulatingPhysics())
            {
                ++SimulatingCount;
            }
            if (FirstPrimitive->Mobility == EComponentMobility::Movable)
            {
                ++MovableCount;
            }
        }
    }
    const bool bGravityOn = ReadBackGravity != 0.0f;
    const float Impact = UEAgentLevelCommandsImpl::GravityImpactScore(bGravityOn, ReadBackGravity, SimulatingCount, MovableCount);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetNumberField(TEXT("applied_gravity_z"), WorldSettings->GlobalGravityZ);
    ResultObj->SetBoolField(TEXT("enable_override"), WorldSettings->bGlobalGravitySet ? true : false);
    ResultObj->SetNumberField(TEXT("gravity_z"), ReadBackGravity);
    ResultObj->SetNumberField(TEXT("previous_gravity_z"), PreviousGravity);
    ResultObj->SetBoolField(TEXT("previous_override"), bPreviousOverride);
    if (bHasPreset)
    {
        ResultObj->SetStringField(TEXT("preset"), PresetName);
    }
    ResultObj->SetStringField(TEXT("world_name"), World ? World->GetName() : FString());
    ResultObj->SetNumberField(TEXT("world_physics_component_count"), PrimitiveCount);
    ResultObj->SetNumberField(TEXT("world_simulate_physics_component_count"), SimulatingCount);
    ResultObj->SetNumberField(TEXT("world_dynamic_physics_component_count"), MovableCount);
    ResultObj->SetNumberField(TEXT("gravity_impact"), Impact);
    ResultObj->SetStringField(TEXT("gravity_impact_rating"), UEAgentLevelCommandsImpl::GravityImpactRating(Impact));
    ResultObj->SetArrayField(TEXT("gravity_recommendations"),
        UEAgentLevelCommandsImpl::GravityAdvice(bGravityOn, ReadBackGravity, SimulatingCount, MovableCount));
    ResultObj->SetBoolField(TEXT("success"), true);

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:set_kill_z - Set the KillZ death plane on the EDITOR world's WorldSettings: any actor falling below this world Z is destroyed, provided the world's bounds checks are enabled (read that flag with get_world_settings, enable_world_bounds_checks -- with it off, KillZ does nothing and the fall looks like a bug). Optionally sets the damage type applied on the kill. Values come back RE-READ off WorldSettings, not echoed. Undoable, but it dirties the MAP: save_level to keep it.
//   @param kill_z - World-space Z height (cm) below which actors get destroyed. Put it under the lowest playable geometry, not at 0.
//   @param kill_z_damage_type - Optional UDamageType class name or full path (e.g. 'DamageType' or '/Script/Engine.DamageType'). Resolved BEFORE anything is written, so a bad name is a clean error and KillZ is left untouched.
//   @returns { applied_kill_z: number, kill_z: number (both re-read off WorldSettings), kill_z_damage_type: string ('None' if unset), success: bool (hardcoded) }

TSharedPtr<FJsonObject> HandleSetKillZ(const TSharedPtr<FJsonObject>& Params)
{
    double KillZValue = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("kill_z"), KillZValue)) { return Missing; }
    FString KillZDamageTypeName;
    const bool bHasDamageType = Params->TryGetStringField(TEXT("kill_z_damage_type"), KillZDamageTypeName)
                                && !KillZDamageTypeName.IsEmpty();

    FString Error;
    AWorldSettings* WorldSettings = GetEditorWorldSettings(Error);
    if (!WorldSettings)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // Optional damage type class resolution (before mutating so we can fail clean)
    UClass* DamageTypeClass = nullptr;
    if (bHasDamageType)
    {
        DamageTypeClass = UEAgentLevelCommandsImpl::FindOrLoadClass<UDamageType>(KillZDamageTypeName);
        if (!DamageTypeClass)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("DamageType class not found: %s. Try a full path like '/Script/Engine.DamageType' or a class name."),
                *KillZDamageTypeName));
        }
        if (!DamageTypeClass->IsChildOf(UDamageType::StaticClass()))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Class is not a DamageType: %s"), *KillZDamageTypeName));
        }
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("Set Kill Z")));
    WorldSettings->Modify();

    WorldSettings->KillZ = static_cast<float>(KillZValue);
    if (DamageTypeClass)
    {
        WorldSettings->KillZDamageType = DamageTypeClass;
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetNumberField(TEXT("applied_kill_z"), WorldSettings->KillZ);
    ResultObj->SetNumberField(TEXT("kill_z"), WorldSettings->KillZ);
    ResultObj->SetStringField(TEXT("kill_z_damage_type"),
        WorldSettings->KillZDamageType ? WorldSettings->KillZDamageType->GetName() : TEXT("None"));
    ResultObj->SetBoolField(TEXT("success"), true);

    return mcp_common::MakeOkReply(ResultObj);
}

// ===========================================================================
// Actor lock + absolute transform.
// All headless-friendly (no Slate).
// ===========================================================================

// MCP:set_actor_lock_location - Lock or unlock an actor in the EDITOR world so viewport dragging cannot move it by accident (AActor::SetLockLocation). Pin landmarks -- PlayerStart, landscape, hand-placed props -- before a batch transform pass. Calls Modify(), so it dirties the actor's package and needs a save_level to persist. Read the flag back with get_actor_lock_location. Does not affect runtime/PIE movement.
//   @param actor_name - Actor's internal name (GetName) or editor label (GetActorLabel); exact match, editor world only.
//   @param locked - True to lock, false to unlock
//   @returns { applied, actor_name, locked }
TSharedPtr<FJsonObject> HandleSetActorLockLocation(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    bool bLocked = true;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredBool(Params, TEXT("locked"), bLocked)) { return Missing; }

    AActor* Actor = mcp_common::FindActorByName(World, ActorName);
    if (!Actor)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    Actor->Modify();
    Actor->SetLockLocation(bLocked);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetStringField(TEXT("actor_name"), ActorName);
    Data->SetBoolField(TEXT("locked"), bLocked);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_actor_lock_location - Read whether an EDITOR-world actor is location-locked in the viewport (AActor::IsLockLocation). The read-back oracle for set_actor_lock_location, and the thing to check first when a transform command reports success but the actor has not moved.
//   @param actor_name - Actor's internal name (GetName) or editor label (GetActorLabel); exact match, editor world only.
//   @returns { actor_name, locked }
TSharedPtr<FJsonObject> HandleGetActorLockLocation(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    TSharedPtr<FJsonObject> Failure;
    AActor* Actor = UEAgentLevelCommandsImpl::ResolveEditorActor(Params, ActorName, Failure);
    if (!Actor)
    {
        return Failure;
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor_name"), ActorName);
    Data->SetBoolField(TEXT("locked"), Actor->IsLockLocation());
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_absolute_transform - Make an EDITOR-world actor's root SceneComponent use ABSOLUTE (world-space) location/rotation/scale instead of inheriting from its parent -- for an attached actor that drifts or shears when its parent moves or is scaled. At least one of the three flags must be supplied (otherwise clean error), and the reply re-reads all three off the component. Read them back with get_absolute_transform; this does NOT call Modify(), so pair it with another dirtying edit or the save may skip the actor.
//   @param actor_name - Actor's internal name (GetName) or editor label (GetActorLabel); exact match, editor world only. Errors if the actor has no RootComponent.
//   @param absolute_location - Optional bool. True = keep world location regardless of the parent; omitted = leave the flag untouched.
//   @param absolute_rotation - Optional bool. True = keep world rotation regardless of the parent; omitted = leave untouched.
//   @param absolute_scale - Optional bool. True = ignore parent scale (stops inherited squash/stretch); omitted = leave untouched.
//   @returns { applied: bool, actor_name: string, absolute_location: bool, absolute_rotation: bool, absolute_scale: bool (all three re-read off the root component) }
TSharedPtr<FJsonObject> HandleSetAbsoluteTransform(const TSharedPtr<FJsonObject>& Params)
{
    FString TargetName;
    TSharedPtr<FJsonObject> Failure;
    AActor* Target = UEAgentLevelCommandsImpl::ResolveEditorActor(Params, TargetName, Failure);
    if (Target == nullptr)
    {
        return Failure;
    }
    USceneComponent* RootScene = Target->GetRootComponent();
    if (RootScene == nullptr)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Actor '%s' has no RootComponent (can't toggle absolute flags)"), *TargetName));
    }

    // Each flag is optional; only the ones present in the request are written.
    int32 FlagsWritten = 0;
    bool bFlagValue = false;
    if (Params->TryGetBoolField(TEXT("absolute_location"), bFlagValue))
    {
        RootScene->SetUsingAbsoluteLocation(bFlagValue);
        ++FlagsWritten;
    }
    if (Params->TryGetBoolField(TEXT("absolute_rotation"), bFlagValue))
    {
        RootScene->SetUsingAbsoluteRotation(bFlagValue);
        ++FlagsWritten;
    }
    if (Params->TryGetBoolField(TEXT("absolute_scale"), bFlagValue))
    {
        RootScene->SetUsingAbsoluteScale(bFlagValue);
        ++FlagsWritten;
    }
    if (FlagsWritten == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Provide at least one of: absolute_location, absolute_rotation, absolute_scale"));
    }

    // The three flags are re-read off the component, not echoed from the request.
    TSharedPtr<FJsonObject> Readback = MakeShared<FJsonObject>();
    Readback->SetBoolField(TEXT("applied"), true);
    Readback->SetStringField(TEXT("actor_name"), TargetName);
    Readback->SetBoolField(TEXT("absolute_location"), RootScene->IsUsingAbsoluteLocation());
    Readback->SetBoolField(TEXT("absolute_rotation"), RootScene->IsUsingAbsoluteRotation());
    Readback->SetBoolField(TEXT("absolute_scale"), RootScene->IsUsingAbsoluteScale());
    return mcp_common::MakeOkReply(Readback);
}

// MCP:get_absolute_transform - Read the absolute (world-space) location/rotation/scale flags of an EDITOR-world actor's root SceneComponent. The read-back oracle for set_absolute_transform, and the first thing to check when an attached actor inherits an unexpected offset or scale from its parent. An actor with no RootComponent answers has_root:false instead of failing.
//   @param actor_name - Actor's internal name (GetName) or editor label (GetActorLabel); exact match, editor world only.
//   @returns { actor_name: string, has_root: bool, absolute_location?: bool, absolute_rotation?: bool, absolute_scale?: bool }
TSharedPtr<FJsonObject> HandleGetAbsoluteTransform(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    TSharedPtr<FJsonObject> Failure;
    AActor* Actor = UEAgentLevelCommandsImpl::ResolveEditorActor(Params, ActorName, Failure);
    if (!Actor)
    {
        return Failure;
    }
    USceneComponent* Root = Actor->GetRootComponent();
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor_name"), ActorName);
    if (Root)
    {
        Data->SetBoolField(TEXT("absolute_location"),  Root->IsUsingAbsoluteLocation());
        Data->SetBoolField(TEXT("absolute_rotation"),  Root->IsUsingAbsoluteRotation());
        Data->SetBoolField(TEXT("absolute_scale"),     Root->IsUsingAbsoluteScale());
        Data->SetBoolField(TEXT("has_root"), true);
    }
    else
    {
        Data->SetBoolField(TEXT("has_root"), false);
    }
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_actor_lifetime - Schedules an actor in the active world (a running PIE/game world if one exists, otherwise the editor world) for automatic destruction after N seconds via FTimerManager::SetTimer. Idempotent: re-running cancels the previous timer and resets the countdown. Used for transient effects (damage numbers, FX) that should auto-cleanup. Runtime/cooked-build safe: resolves the world via GEngine's world contexts, not GEditor, so it also works in packaged builds.
//   @param actor_name - Display label or internal name of the target actor (required)
//   @param seconds - Seconds until the actor is destroyed (required, >0)
//   @returns { actor_name: string, seconds: number, scheduled: bool, replaced_previous_timer: bool }
TSharedPtr<FJsonObject> HandleSetActorLifetime(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }

    double Seconds = 0.0;
    if (!Params->TryGetNumberField(TEXT("seconds"), Seconds) || Seconds <= 0.0)
        return mcp_common::MakeErrorReply(TEXT("Missing or invalid 'seconds' (must be > 0)"));

    UWorld* World = mcp_common::GetActiveWorld();
    if (!World)
        return mcp_common::MakeErrorReply(TEXT("Failed to get active world (no PIE/game world running and no editor world available)"));

    AActor* Actor = mcp_common::FindActorByName(World, ActorName);
    if (!Actor)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor '%s' not found"), *ActorName));

    FTimerManager& TM = World->GetTimerManager();
    bool bHadPrevious = false;
    if (FTimerHandle* Existing = ActorLifetimeTimers.Find(Actor))
    {
        if (TM.IsTimerActive(*Existing))
        {
            TM.ClearTimer(*Existing);
            bHadPrevious = true;
        }
    }

    FTimerHandle NewHandle;
    TWeakObjectPtr<AActor> WeakActor(Actor);
    TM.SetTimer(NewHandle, FTimerDelegate::CreateLambda([WeakActor]()
    {
        if (AActor* A = WeakActor.Get())
        {
            A->Destroy();
        }
    }), static_cast<float>(Seconds), /*bLoop*/ false);

    ActorLifetimeTimers.Add(Actor, NewHandle);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetNumberField(TEXT("seconds"), Seconds);
    Result->SetBoolField(TEXT("scheduled"), true);
    Result->SetBoolField(TEXT("replaced_previous_timer"), bHadPrevious);
    return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// set_pie_window_size
//
// Why: start_play_in_editor has no size parameter, so without this a UI can only be checked at
// whatever resolution the window has -- and the classic UI bug (overlapping at 1280x720
// what fits at 1920x1080) cannot be reproduced headless.
// ---------------------------------------------------------------------------
// MCP:set_pie_window_size - Set the size of the Play-In-Editor window, so a UI can be checked at more than whatever resolution the window happens to have. start_play_in_editor takes no size parameter, so without this the classic UI bug -- fine at 1920x1080, overlapping at 1280x720 -- cannot be reproduced headless. Writes ULevelEditorPlaySettings, the same setting the editor's Play dropdown edits, and it applies to the NEXT PIE session: one already running keeps its window. For a single Widget Blueprint you do not need PIE at all -- render_widget_preview takes width/height/scale directly.
//   @param width [int!] - Window width in pixels (clamped to 160..7680).
//   @param height [int!] - Window height in pixels (clamped to 120..4320).
//   @param center [bool] - Center the new window on screen (default true).
//   @returns { width: int, height: int, center: bool, applies_to: string, pie_currently_active: bool } -- applies_to is always "next PIE session"; WARNING: pie_currently_active:true is the warning that what is on screen RIGHT NOW is not what you just set.
TSharedPtr<FJsonObject> HandleSetPieWindowSize(const TSharedPtr<FJsonObject>& Params)
{
    double W = 0.0, H = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("width"), W)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("height"), H)) { return Missing; }

    const int32 Width  = FMath::Clamp(FMath::RoundToInt(W), 160, 7680);
    const int32 Height = FMath::Clamp(FMath::RoundToInt(H), 120, 4320);

    bool bCenter = true;
    Params->TryGetBoolField(TEXT("center"), bCenter);

    ULevelEditorPlaySettings* PlaySettings = GetMutableDefault<ULevelEditorPlaySettings>();
    if (!PlaySettings)
    {
        return mcp_common::MakeErrorReply(TEXT("ULevelEditorPlaySettings is unavailable"));
    }

    PlaySettings->NewWindowWidth  = Width;
    PlaySettings->NewWindowHeight = Height;
    PlaySettings->CenterNewWindow = bCenter ? 1 : 0;
    PlaySettings->PostEditChange();
    PlaySettings->SaveConfig();

    // If PIE is running, what was just written is NOT what is on screen: saying so here avoids the
    // false negative of measuring a layout against a resolution other than the one requested.
    const bool bPieActive = GEditor && GEditor->PlayWorld != nullptr;

    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetNumberField(TEXT("width"), Width);
    R->SetNumberField(TEXT("height"), Height);
    R->SetBoolField(TEXT("center"), bCenter);
    R->SetStringField(TEXT("applies_to"), TEXT("next PIE session"));
    R->SetBoolField(TEXT("pie_currently_active"), bPieActive);
    return mcp_common::MakeOkReply(R);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("capture_pie_screenshot"), &HandleCapturePieScreenshot },
    { TEXT("get_absolute_transform"), &HandleGetAbsoluteTransform },
    { TEXT("get_actor_blueprint"), &HandleGetActorBlueprint },
    { TEXT("get_actor_components"), &HandleGetActorComponents },
    { TEXT("get_actor_lock_location"), &HandleGetActorLockLocation },
    { TEXT("get_actors_by_class"), &HandleGetActorsByClass },
    { TEXT("get_actors_in_radius"), &HandleGetActorsInRadius },
    { TEXT("get_lighting_in_area"), &HandleGetLightingInArea },
    { TEXT("get_navigation_data"), &HandleGetNavigationData },
    { TEXT("get_world_settings"), &HandleGetWorldSettings },
    { TEXT("inject_analog_input"), &HandleInjectAnalogInput },
    { TEXT("inject_key_input"), &HandleInjectKeyInput },
    { TEXT("inject_mouse_button"), &HandleInjectMouseButton },
    { TEXT("is_play_in_editor_active"), &HandleIsPlayInEditorActive },
    { TEXT("possess_pawn"), &HandlePossessPawn },
    { TEXT("set_absolute_transform"), &HandleSetAbsoluteTransform },
    { TEXT("set_actor_lifetime"), &HandleSetActorLifetime },
    { TEXT("set_actor_lock_location"), &HandleSetActorLockLocation },
    { TEXT("set_control_rotation"), &HandleSetControlRotation },
    { TEXT("set_kill_z"), &HandleSetKillZ },
    { TEXT("set_pie_window_size"), &HandleSetPieWindowSize },
    { TEXT("set_player_controller_class"), &HandleSetPlayerControllerClass },
    { TEXT("set_view_target"), &HandleSetViewTarget },
    { TEXT("set_world_gravity"), &HandleSetWorldGravity },
    { TEXT("spawn_navmesh_bounds_volume"), &HandleSpawnNavMeshBoundsVolume },
    { TEXT("start_play_in_editor"), &HandleStartPlayInEditor },
    { TEXT("stop_play_in_editor"), &HandleStopPlayInEditor },
    { TEXT("wait_for_pie_completion"), &HandleWaitForPieCompletion },
};
} // namespace UEAgentLevelCommands
