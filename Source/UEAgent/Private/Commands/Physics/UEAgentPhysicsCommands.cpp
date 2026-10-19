#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/McpEnumLabels.h"
#include "Commands/Level/PrimitiveComponentUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h" // AWorldSettings::bWorldGravitySet (get_physics_settings)
#include "EngineUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyPortFlags.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Blueprint/BlueprintSupport.h"
#include "Engine/Selection.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "EditorAssetLibrary.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Misc/Timespan.h"
#include "Components/PrimitiveComponent.h"
#include "Components/MeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "UnrealEngine.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "PhysicsEngine/BodySetup.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/PostProcessVolume.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "PhysicsEngine/PhysicsThruster.h"
#include "PhysicsEngine/RadialForceComponent.h"
#include "CollisionQueryParams.h"
// Impulse + ISM CRUD
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Runtime/Launch/Resources/Version.h"
#include "HAL/PlatformProperties.h"
// Spatial queries
#include "CollisionShape.h"
#include "Engine/OverlapResult.h"
#include "Engine/HitResult.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#define LOCTEXT_NAMESPACE "UEAgentPhysics"

namespace UEAgentPhysicsCommands
{

// MCP-DOMAIN: Physics

namespace
{
    /** Resolves a channel name ("ECC_Camera" or "Camera") to ECollisionChannel.
     *  Same recipe as UEAgentCameraCommands.cpp's probe_channel resolver. */
    bool WH4ResolveCollisionChannel(const FString& ChannelName, ECollisionChannel& OutChannel, FString& OutError)
    {
        UEnum* ChannelEnum = StaticEnum<ECollisionChannel>();
        if (!ChannelEnum)
        {
            OutError = TEXT("StaticEnum<ECollisionChannel>() returned null");
            return false;
        }

        FString Canonical = ChannelName;
        if (!Canonical.StartsWith(TEXT("ECC_")))
        {
            Canonical = TEXT("ECC_") + Canonical;
        }
        int64 EnumVal = ChannelEnum->GetValueByNameString(Canonical);

        if (EnumVal == INDEX_NONE)
        {
            for (int32 i = 0; i < ChannelEnum->NumEnums() - 1; ++i) // skip _MAX
            {
                const FString Display = ChannelEnum->GetDisplayNameTextByIndex(i).ToString();
                if (Display.Equals(ChannelName, ESearchCase::IgnoreCase))
                {
                    EnumVal = ChannelEnum->GetValueByIndex(i);
                    break;
                }
            }
        }

        if (EnumVal == INDEX_NONE)
        {
            OutError = FString::Printf(TEXT("Unknown collision channel '%s'. Use one of: ECC_Visibility, ECC_Camera, ECC_WorldStatic, ECC_WorldDynamic, ECC_Pawn, ECC_PhysicsBody, etc."), *ChannelName);
            return false;
        }
        OutChannel = static_cast<ECollisionChannel>(EnumVal);
        return true;
    }

    /**
     * Resolve the world a scene query should run against from an optional "world" param.
     *  - absent / "editor" (default): the editor world.
     *  - "server" / "client" / "pie:N": the addressed PIE world (canonical resolver).
     *  - "pie" / "game": the live PIE/Game world. Editor-world traces do NOT hit World-Partition/OFPA
     *    actors whose collision only cooks in the PIE physics scene, so verifying obstacle collision
     *    requires the PIE world. Errors clearly (listing the worlds that DO exist) otherwise.
     * OutWorldMode receives the selector actually used. Returns nullptr and fills OutError on failure.
     */
    UWorld* WH4ResolveQueryWorld(const TSharedPtr<FJsonObject>& Params, FString& OutError, FString& OutWorldMode)
    {
        // CANONICAL WORLD RESOLUTION. A trace on UEDPIE_0 while another tool reads UEDPIE_1
        // gives two correct readings of two different worlds that look like a game bug, so this
        // is told WHICH world to trace ('server'/'client'/'pie:N') and stamps which one it used.
        // Default: 'editor'; 'game' is an alias of 'pie'.
        UWorld* Resolved = mcp_common::ResolveWorldSelector(Params, TEXT("editor"), OutWorldMode, OutError);
        if (Resolved)
        {
            return Resolved;
        }

        // An EWorldType::Game world (a cooked/-game process) is accepted too. The canonical
        // 'pie' selector only scans PIE contexts, so that case is handled here rather than
        // silently lost -- ActiveGameWorld() is the Game answer of the canonical
        // PIE -> Game -> editor resolver, the only one that is new information here.
        if (!OutWorldMode.Equals(TEXT("editor")))
        {
            if (UWorld* Active = mcp_common::ActiveGameWorld())
            {
                OutError.Reset();
                return Active;
            }
        }
        return nullptr;
    }

    /** Finds ActorName (object name or label) in the editor world; null on success, else the error reply. */
    TSharedPtr<FJsonObject> Physics_FindEditorActor(const FString& ActorName, AActor*& OutActor)
    {
        UWorld* World = nullptr;
        if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }
        OutActor = mcp_common::FindActorByName(World, ActorName);
        if (!OutActor)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor '%s' not found"), *ActorName));
        }
        return nullptr;
    }

    /** Ignores, in QueryParams, every actor of World that the optional "ignore_actors" param names. */
    void Physics_IgnoreListedActors(const TSharedPtr<FJsonObject>& Params, UWorld* World, FCollisionQueryParams& QueryParams)
    {
        const TArray<TSharedPtr<FJsonValue>>* IgnoreActorsArr = nullptr;
        if (Params->TryGetArrayField(TEXT("ignore_actors"), IgnoreActorsArr) && IgnoreActorsArr)
        {
            for (const TSharedPtr<FJsonValue>& V : *IgnoreActorsArr)
            {
                if (AActor* IgnoreActor = mcp_common::FindActorByName(World, V->AsString()))
                {
                    QueryParams.AddIgnoredActor(IgnoreActor);
                }
            }
        }
    }

    /** The origin, extent, min and max fields of a bounds reply, each an {x,y,z} object. */
    void Physics_AddBoxFields(const TSharedPtr<FJsonObject>& Result, const FVector& Origin, const FVector& Extent,
        const FVector& Min, const FVector& Max)
    {
        Result->SetObjectField(TEXT("origin"), mcp_common::VectorToJsonObject(Origin));
        Result->SetObjectField(TEXT("extent"), mcp_common::VectorToJsonObject(Extent));
        Result->SetObjectField(TEXT("min"), mcp_common::VectorToJsonObject(Min));
        Result->SetObjectField(TEXT("max"), mcp_common::VectorToJsonObject(Max));
    }
}

// Helpers of the read-only census commands below (get_physics_settings and
// get_collision_info).
namespace UEAgentPhysicsCommandsImpl
{
    /** Both census commands read the engine's first world context and nothing else. */
    UWorld* CensusWorld()
    {
        return mcp_common::GetFirstContextWorld();
    }

    /** Only the four stock object channels are named; every other channel is "Unknown". */
    const TCHAR* ObjectChannelLabel(ECollisionChannel Channel)
    {
        switch (Channel)
        {
        case ECC_WorldStatic:  return TEXT("WorldStatic");
        case ECC_WorldDynamic: return TEXT("WorldDynamic");
        case ECC_Pawn:         return TEXT("Pawn");
        case ECC_PhysicsBody:  return TEXT("PhysicsBody");
        default:               return TEXT("Unknown");
        }
    }

    const TCHAR* ResponseLabel(ECollisionResponse Response)
    {
        switch (Response)
        {
        case ECR_Block:   return TEXT("Block");
        case ECR_Ignore:  return TEXT("Ignore");
        case ECR_Overlap: return TEXT("Overlap");
        default:          return TEXT("Unknown");
        }
    }

    /** The probe modes have no label of their own and come out as "Unknown". */
    const TCHAR* CollisionModeLabel(ECollisionEnabled::Type Mode)
    {
        switch (Mode)
        {
        case ECollisionEnabled::QueryAndPhysics: return TEXT("QueryAndPhysics");
        case ECollisionEnabled::QueryOnly:       return TEXT("QueryOnly");
        case ECollisionEnabled::PhysicsOnly:     return TEXT("PhysicsOnly");
        case ECollisionEnabled::NoCollision:     return TEXT("NoCollision");
        default:                                 return TEXT("Unknown");
        }
    }

    /** Thresholds shared by both "*_complexity_rating" fields. */
    const TCHAR* ComplexityLabel(float Score)
    {
        if (Score >= 0.8f)
        {
            return TEXT("High");
        }
        if (Score >= 0.6f)
        {
            return TEXT("Medium");
        }
        return Score >= 0.4f ? TEXT("Low") : TEXT("Minimal");
    }

    /** One line of heuristic advice, reported when bApplies. */
    struct FAdviceLine
    {
        bool bApplies;
        const TCHAR* Text;
    };

    /** The text of every line that applies, in table order. */
    template <SIZE_T Count>
    TArray<TSharedPtr<FJsonValue>> ApplicableAdvice(const FAdviceLine (&Lines)[Count])
    {
        TArray<TSharedPtr<FJsonValue>> Advice;
        for (const FAdviceLine& Line : Lines)
        {
            if (Line.bApplies)
            {
                Advice.Add(MakeShared<FJsonValueString>(Line.Text));
            }
        }
        return Advice;
    }

    /** Quality bucket of the project-wide MaxPhysicsDeltaTime. */
    const TCHAR* QualityLabelFor(float MaxDeltaTime)
    {
        if (MaxDeltaTime <= 0.033f)
        {
            return TEXT("High");
        }
        return MaxDeltaTime >= 0.066f ? TEXT("Low") : TEXT("Medium");
    }

    /** Numeric companion of QualityLabelFor. Its middle band is closed on the right, so a
     *  delta time of exactly 0.066 is labelled "Low" yet still scores 0.7. */
    float QualityScoreFor(float MaxDeltaTime)
    {
        if (MaxDeltaTime <= 0.033f)
        {
            return 0.9f;
        }
        return MaxDeltaTime <= 0.066f ? 0.7f : 0.5f;
    }

    /** Totals over the FIRST primitive component of every actor of the census world. */
    struct FPrimitiveCensus
    {
        int32 Total = 0;
        int32 StaticMeshes = 0;
        int32 SkeletalMeshes = 0;
        int32 StaticMobility = 0;
        int32 StationaryMobility = 0;
        int32 MovableMobility = 0;
        int32 Colliding = 0;
        int32 NonColliding = 0;
        int32 Simulating = 0;
        int32 OverlapEvents = 0;
        float MassSum = 0.0f;
        float MassMax = 0.0f;
        float MassMin = FLT_MAX;
        TArray<TSharedPtr<FJsonValue>> Rows;
    };

    /** Counts one primitive into the census and appends its detail row. */
    void AddToCensus(FPrimitiveCensus& Census, UPrimitiveComponent* Prim)
    {
        const bool bColliding = Prim->GetCollisionEnabled() != ECollisionEnabled::NoCollision;
        const bool bSimulating = Prim->IsSimulatingPhysics();
        const bool bOverlapEvents = Prim->GetGenerateOverlapEvents();
        const EComponentMobility::Type Mobility = Prim->Mobility.GetValue();

        ++Census.Total;
        if (Prim->IsA<UStaticMeshComponent>())
        {
            ++Census.StaticMeshes;
        }
        if (Prim->IsA<USkeletalMeshComponent>())
        {
            ++Census.SkeletalMeshes;
        }
        switch (Mobility)
        {
        case EComponentMobility::Static:     ++Census.StaticMobility; break;
        case EComponentMobility::Stationary: ++Census.StationaryMobility; break;
        case EComponentMobility::Movable:    ++Census.MovableMobility; break;
        default: break;
        }
        if (bColliding)
        {
            ++Census.Colliding;
        }
        else
        {
            ++Census.NonColliding;
        }
        Census.Simulating += bSimulating ? 1 : 0;
        Census.OverlapEvents += bOverlapEvents ? 1 : 0;

        // A body that does not simulate has no runtime mass: asking GetMass() would only log a
        // warning and answer 0, so it is not asked and the body adds a zero mass instead.
        const float Mass = bSimulating ? Prim->GetMass() : 0.0f;
        Census.MassSum += Mass;
        Census.MassMax = FMath::Max(Census.MassMax, Mass);
        Census.MassMin = FMath::Min(Census.MassMin, Mass);

        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("component_name"), Prim->GetName());
        Row->SetStringField(TEXT("component_class"), Prim->GetClass()->GetName());
        Row->SetNumberField(TEXT("component_mass"), Mass);
        Row->SetBoolField(TEXT("component_simulate_physics"), bSimulating);
        Row->SetBoolField(TEXT("component_generate_overlap_events"), bOverlapEvents);
        Row->SetBoolField(TEXT("component_generate_hit_events"), false);
        Row->SetStringField(TEXT("component_mobility"), mcp_common::MobilityLabel(Mobility));
        Row->SetStringField(TEXT("component_collision_enabled"), bColliding ? TEXT("True") : TEXT("False"));
        Row->SetStringField(TEXT("component_collision_type"), ObjectChannelLabel(Prim->GetCollisionObjectType()));
        Row->SetObjectField(TEXT("component_position"), mcp_common::VectorToJsonObject(Prim->GetComponentLocation()));
        Row->SetObjectField(TEXT("component_rotation"), mcp_common::RotatorToJsonObject(Prim->GetComponentRotation()));
        Row->SetObjectField(TEXT("component_scale"), mcp_common::VectorToJsonObject(Prim->GetComponentScale()));
        Census.Rows.Add(MakeShared<FJsonValueObject>(Row));
    }

    /** Scans actors in iteration order and returns the first one whose FIRST primitive
     *  component has the wanted name (FString equality, so case-insensitive). */
    UPrimitiveComponent* FindFirstPrimitiveNamed(UWorld* World, const FString& Name)
    {
        for (TActorIterator<AActor> Walker(World); Walker; ++Walker)
        {
            UPrimitiveComponent* Candidate = Walker->FindComponentByClass<UPrimitiveComponent>();
            if (Candidate != nullptr && Candidate->GetName().Equals(Name, ESearchCase::IgnoreCase))
            {
                return Candidate;
            }
        }
        return nullptr;
    }
}

// MCP:get_physics_settings - One-shot physics census of the current world: UPhysicsSettings substepping/delta-time values, world gravity_z, and a walk of every actor counting collision-enabled vs disabled, simulating bodies, mobility split and mass totals, plus a heuristic quality label and a recommendations list. Use it for a broad "what is the physics state of this level" sweep instead of hand-writing the actor loop in Python; for ONE component's collision use get_collision_info, and to change gravity use set_world_gravity / set_physics_gravity. Read the numbers with three caveats visible in the code: physics_engine is a hardcoded "PhysX" literal (UE5 actually runs Chaos), the walk takes only the FIRST primitive component per actor so multi-component actors are undercounted, and generate_hit_events is always 0 (never populated).
//   @returns { physics_settings_found: bool, physics_engine: string (hardcoded "PhysX"), physics_quality: string (heuristic from max_physics_delta_time), max_physics_delta_time, max_substep_delta_time, max_substeps, world_name, world_gravity_z, world_gravity_enabled, per-component counts (collision enabled/disabled, simulating, static/stationary/movable, overlap events), mass totals (only simulating bodies contribute), recommendations: [string], complexity score }

TSharedPtr<FJsonObject> HandleGetPhysicsSettings(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentPhysicsCommandsImpl;

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetBoolField(TEXT("physics_settings_found"), true);
    Report->SetStringField(TEXT("physics_settings_message"), TEXT("Physics settings retrieved successfully"));

    // --- Project solver settings. The engine label is a fixed string, not a query. ---
    const UPhysicsSettings* Solver = UPhysicsSettings::Get();
    const FString Quality = Solver ? Impl::QualityLabelFor(Solver->MaxPhysicsDeltaTime) : TEXT("Unknown");
    Report->SetStringField(TEXT("physics_engine"), TEXT("PhysX"));
    Report->SetStringField(TEXT("physics_quality"), Quality);
    Report->SetStringField(TEXT("physics_quality_level"), Quality);
    Report->SetStringField(TEXT("physics_quality_preset"), Quality);
    Report->SetBoolField(TEXT("physics_quality_auto"), false);
    Report->SetBoolField(TEXT("physics_enabled"), Solver != nullptr);
    Report->SetStringField(TEXT("physics_mode"), Solver ? TEXT("3D Physics") : TEXT("Unknown"));
    Report->SetNumberField(TEXT("physics_quality_score"), Solver ? Impl::QualityScoreFor(Solver->MaxPhysicsDeltaTime) : 0.0f);
    if (Solver)
    {
        Report->SetNumberField(TEXT("max_physics_delta_time"), Solver->MaxPhysicsDeltaTime);
        Report->SetNumberField(TEXT("max_substep_delta_time"), Solver->MaxSubstepDeltaTime);
        Report->SetNumberField(TEXT("max_substeps"), Solver->MaxSubsteps);
    }

    // --- The world being counted. ---
    UWorld* World = Impl::CensusWorld();
    if (World)
    {
        const AWorldSettings* WorldSettings = World->GetWorldSettings();
        Report->SetStringField(TEXT("world_name"), World->GetName());
        Report->SetBoolField(TEXT("world_physics_enabled"), true);
        Report->SetBoolField(TEXT("world_gravity_enabled"), WorldSettings != nullptr && WorldSettings->bWorldGravitySet);
        Report->SetNumberField(TEXT("world_gravity_z"), World->GetGravityZ());
    }

    // --- Census: one primitive (the first found) per actor. ---
    Impl::FPrimitiveCensus Census;
    for (AActor* Actor : TActorRange<AActor>(World))
    {
        if (UPrimitiveComponent* Prim = Actor->FindComponentByClass<UPrimitiveComponent>())
        {
            Impl::AddToCensus(Census, Prim);
        }
    }

    struct FCountField
    {
        const TCHAR* Name;
        int32 Value;
    };
    const FCountField CountFields[] =
    {
        { TEXT("physics_component_count"),                 Census.Total },
        { TEXT("static_mesh_component_count"),             Census.StaticMeshes },
        { TEXT("skeletal_mesh_component_count"),           Census.SkeletalMeshes },
        { TEXT("static_physics_component_count"),          Census.StaticMobility },
        { TEXT("dynamic_physics_component_count"),         Census.MovableMobility },
        { TEXT("kinematic_physics_component_count"),       Census.StationaryMobility },
        { TEXT("collision_enabled_component_count"),       Census.Colliding },
        { TEXT("collision_disabled_component_count"),      Census.NonColliding },
        { TEXT("simulate_physics_component_count"),        Census.Simulating },
        { TEXT("generate_overlap_events_component_count"), Census.OverlapEvents },
        // The hit-event flag is never read, so this count stays at zero.
        { TEXT("generate_hit_events_component_count"),     0 },
    };
    for (const FCountField& Field : CountFields)
    {
        Report->SetNumberField(Field.Name, Field.Value);
    }

    const float MassAverage = Census.Total > 0 ? Census.MassSum / Census.Total : 0.0f;
    Report->SetNumberField(TEXT("total_mass"), Census.MassSum);
    Report->SetNumberField(TEXT("average_mass"), MassAverage);
    Report->SetNumberField(TEXT("max_mass"), Census.MassMax);
    Report->SetNumberField(TEXT("min_mass"), Census.MassMin == FLT_MAX ? 0.0f : Census.MassMin);
    Report->SetArrayField(TEXT("physics_component_details"), Census.Rows);

    // --- Build / platform identity (architecture and OS family are not exposed: fixed labels). ---
    Report->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
    Report->SetStringField(TEXT("platform_name"), FPlatformProperties::PlatformName());
    Report->SetStringField(TEXT("platform_architecture"), TEXT("Unknown"));
    Report->SetStringField(TEXT("platform_os"), TEXT("Unknown"));

    // --- Fixed capability flags and nominal tuning numbers (constants, not measurements). ---
    struct FFlagField
    {
        const TCHAR* Name;
        bool bValue;
    };
    const FFlagField CapabilityFlags[] =
    {
        { TEXT("physics_3d_enabled"), true },
        { TEXT("physics_gravity_enabled"), true },
        { TEXT("physics_collision_enabled"), true },
        { TEXT("physics_simulation_enabled"), true },
        { TEXT("physics_constraints_enabled"), true },
        { TEXT("physics_joints_enabled"), true },
        { TEXT("physics_ragdoll_enabled"), true },
        { TEXT("physics_fluid_enabled"), false },
        { TEXT("physics_cloth_enabled"), false },
        { TEXT("physics_soft_body_enabled"), false },
    };
    for (const FFlagField& Flag : CapabilityFlags)
    {
        Report->SetBoolField(Flag.Name, Flag.bValue);
    }

    struct FTuningField
    {
        const TCHAR* Name;
        float Value;
    };
    const FTuningField NominalTuning[] =
    {
        { TEXT("physics_target_fps"), 60.0f },
        { TEXT("physics_substep_count"), 1.0f },
        { TEXT("physics_max_substep_delta_time"), 0.016667f },
        { TEXT("physics_max_delta_time"), 0.033333f },
        { TEXT("physics_solver_iterations"), 8.0f },
        { TEXT("physics_solver_velocity_iterations"), 1.0f },
    };
    for (const FTuningField& Tuning : NominalTuning)
    {
        Report->SetNumberField(Tuning.Name, Tuning.Value);
    }

    // --- Heuristic advice, in a fixed order. ---
    const Impl::FAdviceLine AdviceLines[] =
    {
        { Census.Total > 100,          TEXT("High physics component count - consider physics optimization") },
        { Census.Simulating > 50,      TEXT("High simulating physics count - consider performance impact") },
        { Census.MovableMobility > 30, TEXT("High dynamic physics count - consider static optimization") },
        { Census.MassMax > 1000.0f,    TEXT("High mass detected - consider mass optimization") },
        { Census.NonColliding > Census.Total * 0.5f, TEXT("High collision disabled ratio - consider collision optimization") },
        { Census.OverlapEvents > 20,   TEXT("High overlap events count - consider event optimization") },
    };
    Report->SetArrayField(TEXT("physics_recommendations"), Impl::ApplicableAdvice(AdviceLines));

    // --- Weighted complexity: four clamped shares, summed in float in this order. ---
    float Complexity = 0.0f;
    if (Census.Total > 0)
    {
        const float Population = static_cast<float>(Census.Total);
        struct FWeightedShare
        {
            float Share;
            float Weight;
        };
        const FWeightedShare Shares[] =
        {
            { Census.Total / 50.0f,                0.3f },
            { Census.Simulating / Population,      0.3f },
            { Census.MovableMobility / Population, 0.2f },
            { Census.OverlapEvents / Population,   0.2f },
        };
        for (const FWeightedShare& Term : Shares)
        {
            Complexity += FMath::Clamp(Term.Share, 0.0f, 1.0f) * Term.Weight;
        }
    }
    Report->SetNumberField(TEXT("physics_complexity_score"), Complexity);
    Report->SetStringField(TEXT("physics_complexity_rating"), Impl::ComplexityLabel(Complexity));

    return Report;
}

// configure_gravity lives in UEAgentLevelCommands.cpp (UEAgentLevelCommands), alongside
// set_world_gravity, which writes the same AWorldSettings::GlobalGravityZ/bGlobalGravitySet fields.

// MCP:get_collision_info - Read ONE placed primitive component's collision setup and physics state: collision enabled mode, per-channel responses (WorldStatic / WorldDynamic / Pawn / PhysicsBody), overlap and hit event flags, simulate-physics, mobility, and the physical material with its friction / restitution / density. The tool to answer "why does this not block / not overlap" without writing Python. Two limits from the code: it searches by component NAME across the world's actors but only inspects each actor's FIRST primitive component, so a secondary component (a second collider, a trigger next to a mesh) reports "not found" even though it exists; and it reads the component's live responses, NOT the profile definition -- for the profile itself use get_collision_profile (which is blind to +EditProfiles overrides), and prove a channel really blocks with raycast_scene.
//   @param component_name - Name of the primitive component to query (must be its actor's first primitive component to be findable).
//   @returns { component_name: string, collision_info_found: bool, collision_info_message: string, collision_enabled: string, collision_type: string, collision_response: string, collision_enabled_state: string, collision_generate_overlap_events: bool, collision_generate_hit_events: bool, collision_simulate_physics: bool, collision_mobility: string, collision_response_world_static: string, collision_response_world_dynamic: string, collision_response_pawn: string, collision_response_physics_body: string, physics_material_is_default: bool, physics_material_name: string, physics_material_path: string, physics_material_surface_type: string, physics_material_friction: float, physics_material_restitution: float, physics_material_density: float, physics_material_raise_mass_to_power: float, physics_material_linear_damping: float (body), physics_material_angular_damping: float (body), physics_material_mass_scale: float (body) }

TSharedPtr<FJsonObject> HandleGetCollisionInfo(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentPhysicsCommandsImpl;

    FString WantedName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("component_name"), WantedName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UWorld* World = Impl::CensusWorld();
    if (!World)
    {
        return mcp_common::MakeErrorReply(TEXT("No world found"));
    }

    // Only the first primitive component of each actor is a candidate (see the annotation).
    UPrimitiveComponent* Prim = Impl::FindFirstPrimitiveNamed(World, WantedName);
    if (!Prim)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Physics component not found: %s"), *WantedName));
    }

    const ECollisionEnabled::Type Mode = Prim->GetCollisionEnabled();
    const bool bSimulating = Prim->IsSimulatingPhysics();
    const bool bOverlapEvents = Prim->GetGenerateOverlapEvents();
    const bool bHitEvents = Prim->BodyInstance.bNotifyRigidBodyCollision;  // "Simulation Generates Hit Events"
    const EComponentMobility::Type Mobility = Prim->Mobility.GetValue();
    const ECollisionResponse VsWorldStatic = Prim->GetCollisionResponseToChannel(ECC_WorldStatic);

    TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
    Info->SetStringField(TEXT("component_name"), WantedName);
    Info->SetBoolField(TEXT("collision_info_found"), true);
    Info->SetStringField(TEXT("collision_info_message"), TEXT("Collision info retrieved successfully"));

    // --- Collision setup as the live component reports it. ---
    Info->SetStringField(TEXT("collision_enabled"), Mode != ECollisionEnabled::NoCollision ? TEXT("True") : TEXT("False"));
    Info->SetStringField(TEXT("collision_enabled_state"), Impl::CollisionModeLabel(Mode));
    Info->SetStringField(TEXT("collision_type"), Impl::ObjectChannelLabel(Prim->GetCollisionObjectType()));
    Info->SetStringField(TEXT("collision_response"), Impl::ResponseLabel(VsWorldStatic));
    Info->SetBoolField(TEXT("collision_generate_overlap_events"), bOverlapEvents);
    Info->SetBoolField(TEXT("collision_generate_hit_events"), bHitEvents);
    Info->SetBoolField(TEXT("collision_simulate_physics"), bSimulating);
    Info->SetStringField(TEXT("collision_mobility"), mcp_common::MobilityLabel(Mobility));

    struct FChannelField
    {
        const TCHAR* Name;
        ECollisionChannel Channel;
    };
    const FChannelField ChannelFields[] =
    {
        { TEXT("collision_response_world_static"),  ECC_WorldStatic },
        { TEXT("collision_response_world_dynamic"), ECC_WorldDynamic },
        { TEXT("collision_response_pawn"),          ECC_Pawn },
        { TEXT("collision_response_physics_body"),  ECC_PhysicsBody },
    };
    for (const FChannelField& Field : ChannelFields)
    {
        Info->SetStringField(Field.Name, Impl::ResponseLabel(Prim->GetCollisionResponseToChannel(Field.Channel)));
    }

    // --- Physical material: the one the body really uses for its simple collision -- the
    //     override, else the body setup's, else the engine default (physics_material_is_default).
    //     Damping and mass scale belong to the body instance, not to the material. ---
    const UPhysicalMaterial* PhysMaterial = Prim->BodyInstance.GetSimplePhysicalMaterial();
    const bool bDefaultMaterial = PhysMaterial == nullptr || (GEngine != nullptr && PhysMaterial == GEngine->DefaultPhysMaterial);
    Info->SetBoolField(TEXT("physics_material_is_default"), bDefaultMaterial);
    Info->SetStringField(TEXT("physics_material_name"), PhysMaterial ? PhysMaterial->GetName() : FString(TEXT("None")));
    Info->SetStringField(TEXT("physics_material_path"), PhysMaterial ? PhysMaterial->GetPathName() : FString(TEXT("None")));
    const UEnum* SurfaceEnum = StaticEnum<EPhysicalSurface>();
    Info->SetStringField(TEXT("physics_material_surface_type"), PhysMaterial && SurfaceEnum
        ? SurfaceEnum->GetDisplayNameTextByValue(PhysMaterial->SurfaceType.GetValue()).ToString()
        : FString(TEXT("None")));
    Info->SetNumberField(TEXT("physics_material_friction"), PhysMaterial ? PhysMaterial->Friction : 0.0f);
    Info->SetNumberField(TEXT("physics_material_restitution"), PhysMaterial ? PhysMaterial->Restitution : 0.0f);
    Info->SetNumberField(TEXT("physics_material_density"), PhysMaterial ? PhysMaterial->Density : 0.0f);
    Info->SetNumberField(TEXT("physics_material_raise_mass_to_power"), PhysMaterial ? PhysMaterial->RaiseMassToPower : 0.0f);
    Info->SetNumberField(TEXT("physics_material_linear_damping"), Prim->BodyInstance.LinearDamping);
    Info->SetNumberField(TEXT("physics_material_angular_damping"), Prim->BodyInstance.AngularDamping);
    Info->SetNumberField(TEXT("physics_material_mass_scale"), Prim->BodyInstance.MassScale);

    // --- The component itself. ---
    const bool bVisible = Prim->IsVisible();
    Info->SetStringField(TEXT("component_class"), Prim->GetClass()->GetName());
    Info->SetNumberField(TEXT("component_mass"), bSimulating ? Prim->GetMass() : 0.0f);
    Info->SetBoolField(TEXT("component_enabled"), Prim->IsActive());
    Info->SetBoolField(TEXT("component_visible"), bVisible);
    Info->SetBoolField(TEXT("component_visible_in_game"), bVisible);
    Info->SetBoolField(TEXT("component_visible_in_editor"), Prim->IsVisibleInEditor());
    Info->SetObjectField(TEXT("component_position"), mcp_common::VectorToJsonObject(Prim->GetComponentLocation()));
    Info->SetObjectField(TEXT("component_rotation"), mcp_common::RotatorToJsonObject(Prim->GetComponentRotation()));
    Info->SetObjectField(TEXT("component_scale"), mcp_common::VectorToJsonObject(Prim->GetComponentScale()));

    if (const AActor* OwnerActor = Prim->GetOwner())
    {
        Info->SetStringField(TEXT("component_owner_name"), OwnerActor->GetName());
        Info->SetStringField(TEXT("component_owner_class"), OwnerActor->GetClass()->GetName());
        Info->SetStringField(TEXT("component_owner_path"), OwnerActor->GetPathName());
        Info->SetObjectField(TEXT("component_owner_position"), mcp_common::VectorToJsonObject(OwnerActor->GetActorLocation()));
        Info->SetObjectField(TEXT("component_owner_rotation"), mcp_common::RotatorToJsonObject(OwnerActor->GetActorRotation()));
    }
    else
    {
        const TCHAR* const OwnerFields[] =
        {
            TEXT("component_owner_name"),
            TEXT("component_owner_class"),
            TEXT("component_owner_path"),
        };
        for (const TCHAR* FieldName : OwnerFields)
        {
            Info->SetStringField(FieldName, TEXT("None"));
        }
    }

    UWorld* PrimWorld = Prim->GetWorld();
    Info->SetStringField(TEXT("component_level"), PrimWorld->GetCurrentLevel()->GetName());
    Info->SetStringField(TEXT("component_world"), PrimWorld->GetName());

    // --- Heuristic advice, in a fixed order. ---
    const Impl::FAdviceLine AdviceLines[] =
    {
        { Mode == ECollisionEnabled::NoCollision,
          TEXT("No collision enabled - consider enabling for physics interaction") },
        { Mode == ECollisionEnabled::QueryOnly && bSimulating,
          TEXT("QueryOnly collision with physics simulation - consider QueryAndPhysics") },
        { Mode == ECollisionEnabled::PhysicsOnly && bOverlapEvents,
          TEXT("PhysicsOnly collision with overlap events - consider QueryAndPhysics") },
        { bOverlapEvents && VsWorldStatic == ECR_Block,
          TEXT("Overlap events with block response - consider Overlap response") },
        { bSimulating && Mobility == EComponentMobility::Static,
          TEXT("Physics simulation with static mobility - consider Movable mobility") },
        { bDefaultMaterial,
          TEXT("No physics material set - consider setting appropriate material") },
    };
    Info->SetArrayField(TEXT("collision_recommendations"), Impl::ApplicableAdvice(AdviceLines));

    // --- Complexity: 0.2 per trait present, summed in float. ---
    const bool Traits[] =
    {
        Mode != ECollisionEnabled::NoCollision,
        bOverlapEvents,
        bHitEvents,
        bSimulating,
        Mobility == EComponentMobility::Movable,
    };
    float Complexity = 0.0f;
    for (const bool bTrait : Traits)
    {
        if (bTrait)
        {
            Complexity += 0.2f;
        }
    }
    Info->SetNumberField(TEXT("collision_complexity_score"), Complexity);
    Info->SetStringField(TEXT("collision_complexity_rating"), Impl::ComplexityLabel(Complexity));

    return Info;
}

// Impulse + ISM CRUD

// MCP:add_impulse_to_actor - Kick a physics body: apply an impulse (or, with vel_change, a mass-independent velocity change) to an actor's primitive component, optionally at a world location for torque via AddImpulseAtLocation, or on a named skeletal bone. Acts on the EDITOR world, where nothing is simulating unless the component has simulate_physics on -- so on a resting editor actor this is a no-op that still reports applied:true; verify with a transform re-read, and for gameplay-time forces drive it in PIE.
//   @param actor_name - Display label or internal name of the target editor-world actor (required)
//   @param component_name - Optional primitive component name (defaults to the actor's RootComponent)
//   @param impulse - [x, y, z] impulse vector in world units (required)
//   @param location - Optional [x, y, z] world location for AddImpulseAtLocation (omit to use AddImpulse)
//   @param bone_name - Optional skeletal bone name to apply the impulse to
//   @param vel_change - Optional; if true, impulse is treated as a velocity change (mass-independent)
//   @returns { actor_name: string, component_name: string, applied: bool }
TSharedPtr<FJsonObject> HandleAddImpulseToActor(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }

    AActor* Actor = nullptr;
    if (TSharedPtr<FJsonObject> Error = Physics_FindEditorActor(ActorName, Actor))
    {
        return Error;
    }

    FString ComponentName;
    Params->TryGetStringField(TEXT("component_name"), ComponentName);

    UPrimitiveComponent* Prim = nullptr;
    FString ComponentError;
    if (!PrimitiveComponentUtils::FindPrimitive(Actor, ActorName, ComponentName, Prim, ComponentError))
    {
        return mcp_common::MakeErrorReply(ComponentError);
    }
    if (!Prim)
    {
        return mcp_common::MakeErrorReply(TEXT("No UPrimitiveComponent resolved for the actor"));
    }

    const FVector Impulse = mcp_common::ReadJsonVector(Params, TEXT("impulse"));

    FString BoneName;
    Params->TryGetStringField(TEXT("bone_name"), BoneName);
    const FName Bone = BoneName.IsEmpty() ? NAME_None : FName(*BoneName);

    bool bVelChange = false;
    Params->TryGetBoolField(TEXT("vel_change"), bVelChange);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetStringField(TEXT("component_name"), Prim->GetName());

    // Choose AddImpulse vs AddImpulseAtLocation based on whether "location" was supplied.
    bool bHasLocation = false;
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Params->TryGetArrayField(TEXT("location"), Arr) && Arr && Arr->Num() >= 3)
        {
            bHasLocation = true;
            const FVector Loc(
                (*Arr)[0]->AsNumber(),
                (*Arr)[1]->AsNumber(),
                (*Arr)[2]->AsNumber());
            Prim->AddImpulseAtLocation(Impulse, Loc, Bone);
        }
    }
    if (!bHasLocation)
    {
        Prim->AddImpulse(Impulse, Bone, bVelChange);
    }

    Result->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:add_instanced_static_mesh_component - Attach an InstancedStaticMeshComponent (ISM) to a placed editor-world actor so you can then scatter many copies of one static mesh through it with add_ism_instance. Idempotent: if the actor already has an ISM it returns that one with already_existed=true instead of adding a second. This is the working alternative to the foliage scatter tools, which crash the editor in this build.
//   @param actor_name - Display label or internal name of the target editor-world actor (required)
//   @param static_mesh_path - Optional /Game/... path to a UStaticMesh asset (defaults to engine cube if absent)
//   @returns { actor_name: string, component_name: string, component_class: string, num_instances: int, already_existed: bool }
TSharedPtr<FJsonObject> HandleAddInstancedStaticMeshComponent(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }

    AActor* Actor = nullptr;
    if (TSharedPtr<FJsonObject> Error = Physics_FindEditorActor(ActorName, Actor))
    {
        return Error;
    }

    FString StaticMeshPath;
    Params->TryGetStringField(TEXT("static_mesh_path"), StaticMeshPath);

    // Idempotency: re-use an existing ISM component on the actor if one exists.
    UInstancedStaticMeshComponent* Existing = Actor->FindComponentByClass<UInstancedStaticMeshComponent>();
    if (Existing)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("actor_name"), ActorName);
        Result->SetStringField(TEXT("component_name"), Existing->GetName());
        Result->SetStringField(TEXT("component_class"), Existing->GetClass()->GetName());
        Result->SetNumberField(TEXT("num_instances"), Existing->GetInstanceCount());
        Result->SetBoolField(TEXT("already_existed"), true);
        return mcp_common::MakeOkReply(Result);
    }

    const FScopedTransaction Transaction(LOCTEXT("AddISMComponent", "MCP: Add InstancedStaticMeshComponent"));
    Actor->Modify();

    UInstancedStaticMeshComponent* ISM = NewObject<UInstancedStaticMeshComponent>(Actor);
    if (!ISM)
        return mcp_common::MakeErrorReply(TEXT("Failed to create UInstancedStaticMeshComponent"));

    ISM->CreationMethod = EComponentCreationMethod::Instance;

    if (!StaticMeshPath.IsEmpty())
    {
        if (UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *StaticMeshPath))
        {
            ISM->SetStaticMesh(Mesh);
        }
    }

    PrimitiveComponentUtils::RegisterInstanceComponent(Actor, ISM);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetStringField(TEXT("component_name"), ISM->GetName());
    Result->SetStringField(TEXT("component_class"), ISM->GetClass()->GetName());
    Result->SetNumberField(TEXT("num_instances"), ISM->GetInstanceCount());
    Result->SetBoolField(TEXT("already_existed"), false);
    return mcp_common::MakeOkReply(Result);
}

namespace
{
    /** Resolve a UInstancedStaticMeshComponent on the actor. Returns the first one if no name specified. */
    UInstancedStaticMeshComponent* ResolveISM(AActor* Actor, const FString& ComponentName, FString& OutComponentName)
    {
        if (!Actor) { return nullptr; }
        TArray<UInstancedStaticMeshComponent*> ISMs;
        Actor->GetComponents<UInstancedStaticMeshComponent>(ISMs);
        if (ISMs.Num() == 0) { return nullptr; }

        if (ComponentName.IsEmpty())
        {
            OutComponentName = ISMs[0]->GetName();
            return ISMs[0];
        }
        for (UInstancedStaticMeshComponent* C : ISMs)
        {
            if (C && C->GetName() == ComponentName)
            {
                OutComponentName = C->GetName();
                return C;
            }
        }
        return nullptr;
    }

    /**
     * Finds ActorName in the editor world and its ISM component named by "component_name" (the first one when absent);
     * null on success, else the error reply. bNameComponentInError adds the requested component name to the error
     * reported when the actor has no such ISM.
     */
    TSharedPtr<FJsonObject> Physics_FindIsm(const TSharedPtr<FJsonObject>& Params, const FString& ActorName, bool bNameComponentInError,
        UInstancedStaticMeshComponent*& OutIsm, FString& OutComponentName)
    {
        AActor* Actor = nullptr;
        if (TSharedPtr<FJsonObject> Error = Physics_FindEditorActor(ActorName, Actor))
        {
            return Error;
        }

        FString ComponentName;
        Params->TryGetStringField(TEXT("component_name"), ComponentName);

        OutIsm = ResolveISM(Actor, ComponentName, OutComponentName);
        if (!OutIsm)
        {
            const FString Matching = (bNameComponentInError && !ComponentName.IsEmpty())
                ? FString::Printf(TEXT(" matching '%s'"), *ComponentName)
                : FString();
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("No UInstancedStaticMeshComponent on actor '%s'%s"), *ActorName, *Matching));
        }
        return nullptr;
    }

    /**
     * Reads the required actor_name and instance_index (>= 0), then finds the actor's ISM component as Physics_FindIsm
     * does; null on success, else the error reply of the first check that failed.
     */
    TSharedPtr<FJsonObject> Physics_ReadIsmInstance(const TSharedPtr<FJsonObject>& Params, UInstancedStaticMeshComponent*& OutIsm,
        FString& OutComponentName, int32& OutInstanceIndex)
    {
        FString ActorName;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }

        double IndexD = 0.0;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("instance_index"), IndexD)) { return Missing; }
        OutInstanceIndex = static_cast<int32>(IndexD);
        if (OutInstanceIndex < 0)
        {
            return mcp_common::MakeErrorReply(TEXT("'instance_index' must be >= 0"));
        }

        return Physics_FindIsm(Params, ActorName, /*bNameComponentInError=*/false, OutIsm, OutComponentName);
    }

    /** The instance transform of "location", "rotation" and "scale" ([1,1,1] unless "scale" holds 3 numbers). */
    FTransform Physics_ReadInstanceTransform(const TSharedPtr<FJsonObject>& Params)
    {
        const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));
        const FRotator Rotation = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));

        FVector Scale(1.0);
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Params->TryGetArrayField(TEXT("scale"), Arr) && Arr && Arr->Num() >= 3)
        {
            Scale = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
        }
        return FTransform(Rotation, Location, Scale);
    }
}

// MCP:add_ism_instance - Place one more instance of an InstancedStaticMeshComponent's mesh at a world transform -- the cheap way to scatter many copies of a mesh (rocks, props, vegetation) without spawning an actor each. Pair it with add_instanced_static_mesh_component to create the component first. Prefer this route over the foliage tools: scatter_foliage_in_box and add_foliage_instances both CRASH the editor in this build, and an ISM is the same code path a foliage actor and PCG use anyway.
//   @param actor_name - Display label or internal name of the actor carrying the ISM component (required)
//   @param component_name - Optional ISM component name (defaults to the first one on the actor)
//   @param location - [x, y, z] world-space translation (required)
//   @param rotation - Optional [pitch, yaw, roll] world rotation (defaults to zero)
//   @param scale - Optional [x, y, z] scale (defaults to [1,1,1])
//   @param world_space - Optional; if true, transform is interpreted in world space (default true)
//   @returns { instance_index: int, num_instances: int }
TSharedPtr<FJsonObject> HandleAddIsmInstance(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }

    UInstancedStaticMeshComponent* ISM = nullptr;
    FString ResolvedCompName;
    if (TSharedPtr<FJsonObject> Error = Physics_FindIsm(Params, ActorName, /*bNameComponentInError=*/true, ISM, ResolvedCompName))
    {
        return Error;
    }

    const FTransform InstanceXform = Physics_ReadInstanceTransform(Params);
    bool bWorldSpace = true;
    Params->TryGetBoolField(TEXT("world_space"), bWorldSpace);

    const int32 InstanceIndex = ISM->AddInstance(InstanceXform, bWorldSpace);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("instance_index"), InstanceIndex);
    Result->SetNumberField(TEXT("num_instances"), ISM->GetInstanceCount());
    Result->SetStringField(TEXT("component_name"), ResolvedCompName);
    return mcp_common::MakeOkReply(Result);
}

// MCP:remove_ism_instance - Delete ONE instance from an InstancedStaticMeshComponent by index. Indices shift after a removal, so remove from the highest index down when deleting several; to empty the component entirely use clear_ism_instances instead. Re-read num_instances from the reply (or get_ism_instance_info) as the oracle -- removed:true alone is not proof of the final count.
//   @param actor_name - Display label or internal name of the actor carrying the ISM component (required)
//   @param component_name - Optional ISM component name
//   @param instance_index - Index of the instance to remove (required, >=0)
//   @returns { removed: bool, num_instances: int }
TSharedPtr<FJsonObject> HandleRemoveIsmInstance(const TSharedPtr<FJsonObject>& Params)
{
    UInstancedStaticMeshComponent* ISM = nullptr;
    FString ResolvedCompName;
    int32 InstanceIndex = INDEX_NONE;
    if (TSharedPtr<FJsonObject> Error = Physics_ReadIsmInstance(Params, ISM, ResolvedCompName, InstanceIndex))
    {
        return Error;
    }

    const bool bRemoved = ISM->RemoveInstance(InstanceIndex);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("removed"), bRemoved);
    Result->SetNumberField(TEXT("num_instances"), ISM->GetInstanceCount());
    Result->SetStringField(TEXT("component_name"), ResolvedCompName);
    return mcp_common::MakeOkReply(Result);
}

// MCP:update_ism_instance_transform - Move, rotate or rescale ONE existing instance of an InstancedStaticMeshComponent in place, by index -- cheaper than remove + re-add and it keeps the index stable. Leave mark_render_state_dirty at its default true or the viewport keeps drawing the old transform.
//   @param actor_name - Display label or internal name of the actor carrying the ISM component (required)
//   @param component_name - Optional ISM component name
//   @param instance_index - Index of the instance to update (required, >=0)
//   @param location - [x, y, z] new translation
//   @param rotation - Optional [pitch, yaw, roll]
//   @param scale - Optional [x, y, z]
//   @param world_space - Optional; defaults true
//   @param mark_render_state_dirty - Optional; default true (engine signature bMarkRenderStateDirty=true)
//   @param teleport - Optional; default false
//   @returns { updated: bool, num_instances: int }
TSharedPtr<FJsonObject> HandleUpdateIsmInstanceTransform(const TSharedPtr<FJsonObject>& Params)
{
    UInstancedStaticMeshComponent* ISM = nullptr;
    FString ResolvedCompName;
    int32 InstanceIndex = INDEX_NONE;
    if (TSharedPtr<FJsonObject> Error = Physics_ReadIsmInstance(Params, ISM, ResolvedCompName, InstanceIndex))
    {
        return Error;
    }

    const FTransform NewXform = Physics_ReadInstanceTransform(Params);

    bool bWorldSpace = true;
    Params->TryGetBoolField(TEXT("world_space"), bWorldSpace);
    bool bMarkRenderStateDirty = true;
    Params->TryGetBoolField(TEXT("mark_render_state_dirty"), bMarkRenderStateDirty);
    bool bTeleport = false;
    Params->TryGetBoolField(TEXT("teleport"), bTeleport);

    const bool bUpdated = ISM->UpdateInstanceTransform(
        InstanceIndex, NewXform, bWorldSpace, bMarkRenderStateDirty, bTeleport);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("updated"), bUpdated);
    Result->SetNumberField(TEXT("num_instances"), ISM->GetInstanceCount());
    Result->SetStringField(TEXT("component_name"), ResolvedCompName);
    return mcp_common::MakeOkReply(Result);
}

// MCP:clear_ism_instances - Wipe ALL instances from an InstancedStaticMeshComponent in one call (the component itself stays). Use it to reset a scatter before re-running it, rather than looping remove_ism_instance. Returns the post-clear num_instances, which should be 0.
//   @param actor_name - Display label or internal name of the actor carrying the ISM component (required)
//   @param component_name - Optional ISM component name
//   @returns { cleared: bool, num_instances: int }
TSharedPtr<FJsonObject> HandleClearIsmInstances(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }

    UInstancedStaticMeshComponent* ISM = nullptr;
    FString ResolvedCompName;
    if (TSharedPtr<FJsonObject> Error = Physics_FindIsm(Params, ActorName, /*bNameComponentInError=*/false, ISM, ResolvedCompName))
    {
        return Error;
    }

    ISM->ClearInstances();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("cleared"), true);
    Result->SetNumberField(TEXT("num_instances"), ISM->GetInstanceCount());
    Result->SetStringField(TEXT("component_name"), ResolvedCompName);
    return mcp_common::MakeOkReply(Result);
}

// MCP:get_ism_instance_info - Count the instances in an InstancedStaticMeshComponent, or read one instance's transform (location / rotation / scale) and per-instance custom data floats. Two modes from one call: omit instance_index for COUNT mode (num_instances + num_custom_data_floats -- the oracle for "did my scatter actually place anything"), pass it for per-instance mode. Reads the component in the editor world, so it needs no PIE.
//   @param actor_name - Display label or internal name of the actor carrying the ISM component (required)
//   @param component_name - Optional ISM component name (defaults to the first one on the actor)
//   @param instance_index - Optional; absent -> count mode; present and >=0 -> per-instance mode (must be a valid index)
//   @param world_space - Optional; if true, transform is returned in world space (default true)
//   @returns count mode { num_instances: int, num_custom_data_floats: int } or instance mode { instance_index: int, num_instances: int, world_space: bool, location: [x,y,z], rotation: [pitch,yaw,roll], scale: [x,y,z], custom_data: [float], num_custom_data_floats: int }
TSharedPtr<FJsonObject> HandleGetIsmInstanceInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }

    UInstancedStaticMeshComponent* ISM = nullptr;
    FString ResolvedCompName;
    if (TSharedPtr<FJsonObject> Error = Physics_FindIsm(Params, ActorName, /*bNameComponentInError=*/false, ISM, ResolvedCompName))
    {
        return Error;
    }

    // Absent instance_index -> count mode; present -> per-instance mode.
    double IndexD = 0.0;
    const bool bHasIndex = Params->TryGetNumberField(TEXT("instance_index"), IndexD);

    if (!bHasIndex)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetNumberField(TEXT("num_instances"), ISM->GetInstanceCount());
        Result->SetNumberField(TEXT("num_custom_data_floats"), ISM->NumCustomDataFloats);
        Result->SetStringField(TEXT("component_name"), ResolvedCompName);
        return mcp_common::MakeOkReply(Result);
    }

    const int32 InstanceIndex = static_cast<int32>(IndexD);
    if (InstanceIndex < 0 || !ISM->IsValidInstance(InstanceIndex))
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Instance index %d out of range (num_instances=%d)"), InstanceIndex, ISM->GetInstanceCount()));

    bool bWorldSpace = true;
    Params->TryGetBoolField(TEXT("world_space"), bWorldSpace);

    FTransform Out;
    ISM->GetInstanceTransform(InstanceIndex, Out, bWorldSpace);

    const FVector Translation = Out.GetTranslation();
    const FRotator RotatorOut = Out.Rotator();
    const FVector ScaleOut = Out.GetScale3D();

    TArray<TSharedPtr<FJsonValue>> LocationArr = mcp_common::VectorToJsonArray(Translation);

    TArray<TSharedPtr<FJsonValue>> RotationArr = mcp_common::RotatorToJsonArray(RotatorOut);

    TArray<TSharedPtr<FJsonValue>> ScaleArr = mcp_common::VectorToJsonArray(ScaleOut);

    // Custom data has no getter: instance i occupies slice [i*N, (i+1)*N) of PerInstanceSMCustomData.
    const int32 NumCustomDataFloats = ISM->NumCustomDataFloats;
    TArray<TSharedPtr<FJsonValue>> CustomDataArr;
    if (NumCustomDataFloats > 0 && ISM->PerInstanceSMCustomData.Num() >= (InstanceIndex + 1) * NumCustomDataFloats)
    {
        const int32 Base = InstanceIndex * NumCustomDataFloats;
        for (int32 i = 0; i < NumCustomDataFloats; ++i)
        {
            CustomDataArr.Add(MakeShared<FJsonValueNumber>(ISM->PerInstanceSMCustomData[Base + i]));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("instance_index"), InstanceIndex);
    Result->SetNumberField(TEXT("num_instances"), ISM->GetInstanceCount());
    Result->SetBoolField(TEXT("world_space"), bWorldSpace);
    Result->SetArrayField(TEXT("location"), LocationArr);
    Result->SetArrayField(TEXT("rotation"), RotationArr);
    Result->SetArrayField(TEXT("scale"), ScaleArr);
    Result->SetArrayField(TEXT("custom_data"), CustomDataArr);
    Result->SetNumberField(TEXT("num_custom_data_floats"), NumCustomDataFloats);
    Result->SetStringField(TEXT("component_name"), ResolvedCompName);
    return mcp_common::MakeOkReply(Result);
}

// Spatial queries with real JSON results (not export-text via call_function).

// MCP:raycast_scene - Line trace / raycast the world and get a REAL JSON hit result -- what actor and component was hit, impact point, normal and distance -- instead of the UE export-text FHitResult blob that call_function on a line-trace UFUNCTION returns, so no Python parsing is needed. Accepts a `world` selector (editor / server / client / pie:N), which matters twice: an EDITOR-world trace does not hit World-Partition/OFPA actor collision (only cooked in the PIE physics scene) and reads as a clean miss, and in co-op a miss on the client world looks identical to a missing collider -- pass "server" or "pie:N" so a hit:false is interpretable. Also the honest way to prove a custom collision CHANNEL blocks: trace the same segment on a known-good channel (Visibility) and on the new one and compare actor/component/Z/distance, because get_collision_profile does not see +EditProfiles overrides.
//   @param start [array!] - World-space [x, y, z] trace start
//   @param end [array!] - World-space [x, y, z] trace end
//   @param trace_channel [string] - Collision channel, canonical ("ECC_Visibility") or friendly ("Visibility", "Camera", or a custom channel name). Defaults to "Visibility". A channel created THIS session is not in the live profile table yet -- restart the editor before trusting a trace on it.
//   @param trace_complex [bool] - Test against complex (per-poly) collision instead of simple. Defaults to false.
//   @param ignore_actors [array] - Actor names/labels to exclude from the trace.
//   @param return_physical_material [bool] - Populate physical_material in the result. Defaults to false.
//   @param world [string] - Which world to trace: absent/"editor" (default) | "server" (the PIE world with authority) | "client" | "pie:N" | "pie" (ambiguous: PlayWorld, else the first PIE context) | "game" (alias of "pie"). Editor-world traces do NOT hit World-Partition/OFPA actors, so pass a live world to verify obstacle collision; with 2+ PIE instances prefer "server"/"pie:N".
//   @returns { hit: bool, blocking_hit: bool, actor_path: string, actor_name: string, component_name: string, location: {x,y,z}, impact_point: {x,y,z}, normal: {x,y,z}, impact_normal: {x,y,z}, distance: float, bone_name: string, physical_material: string, face_index: int, world: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }
//   @hints read-only
TSharedPtr<FJsonObject> HandleRaycastScene(const TSharedPtr<FJsonObject>& Params)
{
    const TArray<TSharedPtr<FJsonValue>>* StartArr = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* EndArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("start"), StartArr) || !StartArr
        || !Params->TryGetArrayField(TEXT("end"), EndArr) || !EndArr)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter(s): start, end (each [x, y, z])"));
    }
    const FVector Start = mcp_common::ReadJsonVector(Params, TEXT("start"));
    const FVector End = mcp_common::ReadJsonVector(Params, TEXT("end"));

    FString ChannelName = TEXT("Visibility");
    Params->TryGetStringField(TEXT("trace_channel"), ChannelName);
    ECollisionChannel Channel = ECC_Visibility;
    FString ChannelError;
    if (!WH4ResolveCollisionChannel(ChannelName, Channel, ChannelError))
    {
        return mcp_common::MakeErrorReply(ChannelError);
    }

    bool bTraceComplex = false;
    Params->TryGetBoolField(TEXT("trace_complex"), bTraceComplex);
    bool bReturnPhysMat = false;
    Params->TryGetBoolField(TEXT("return_physical_material"), bReturnPhysMat);

    FString WorldError, WorldMode;
    UWorld* World = WH4ResolveQueryWorld(Params, WorldError, WorldMode);
    if (!World)
    {
        return mcp_common::MakeErrorReply(WorldError);
    }

    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(McpRaycastScene), bTraceComplex);
    QueryParams.bReturnPhysicalMaterial = bReturnPhysMat;
    Physics_IgnoreListedActors(Params, World, QueryParams);

    FHitResult Hit;
    const bool bHit = World->LineTraceSingleByChannel(Hit, Start, End, Channel, QueryParams);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("hit"), bHit);
    Result->SetBoolField(TEXT("blocking_hit"), Hit.bBlockingHit);
    if (bHit)
    {
        AActor* HitActor = Hit.GetActor();
        UPrimitiveComponent* HitComponent = Hit.GetComponent();
        Result->SetStringField(TEXT("actor_path"), HitActor ? HitActor->GetPathName() : FString());
        Result->SetStringField(TEXT("actor_name"), HitActor ? HitActor->GetName() : FString());
        Result->SetStringField(TEXT("component_name"), HitComponent ? HitComponent->GetName() : FString());
        Result->SetObjectField(TEXT("location"), mcp_common::VectorToJsonObject(Hit.Location));
        Result->SetObjectField(TEXT("impact_point"), mcp_common::VectorToJsonObject(Hit.ImpactPoint));
        Result->SetObjectField(TEXT("normal"), mcp_common::VectorToJsonObject(Hit.Normal));
        Result->SetObjectField(TEXT("impact_normal"), mcp_common::VectorToJsonObject(Hit.ImpactNormal));
        Result->SetNumberField(TEXT("distance"), Hit.Distance);
        Result->SetStringField(TEXT("bone_name"), Hit.BoneName.ToString());
        Result->SetStringField(TEXT("physical_material"),
            Hit.PhysMaterial.IsValid() ? Hit.PhysMaterial->GetName() : FString());
        Result->SetNumberField(TEXT("face_index"), Hit.FaceIndex);
    }
    // A "hit: false" is only interpretable if you know WHICH world was traced.
    Result->SetStringField(TEXT("world"), WorldMode);
    mcp_common::AddWorldEvidence(Result, World);
    return mcp_common::MakeOkReply(Result);
}

// MCP:overlap_scene - Sphere / box / capsule overlap query against the world: "what is inside this volume?", returning one JSON entry per overlapping actor+component instead of export-text, so no Python is needed to parse it. Use it over raycast_scene when the question is proximity/containment rather than line of sight -- e.g. is the pawn actually inside a water body's real collision, is anything blocking a build spot. Accepts the same `world` selector (editor / server / client / pie:N): an editor-world query does NOT see World-Partition/OFPA actor collision, so pass a live PIE world, and in co-op say WHICH one or an empty result is uninterpretable.
//   @param shape [string!] - "sphere" | "box" | "capsule"
//   @param location [array!] - World-space [x, y, z] shape center
//   @param radius [number] - Sphere/capsule radius (required for those shapes)
//   @param half_extent [array] - Box HALF-extent [x, y, z], not the full size (required for box)
//   @param half_height [number] - Capsule half-height (required for capsule)
//   @param trace_channel [string] - Collision channel, canonical ("ECC_Visibility") or friendly ("Visibility"). Defaults to "Visibility".
//   @param ignore_actors [array] - Actor names/labels to exclude from the overlap.
//   @param world [string] - Which world to query: absent/"editor" (default) | "server" (the PIE world with authority) | "client" | "pie:N" | "pie" (ambiguous) | "game" (alias of "pie"). Pass a live world to hit World-Partition/OFPA actor collision (only cooked in the PIE physics scene).
//   @returns { overlaps: [{actor_path, actor_name, component_name, item_index, blocking}], count: int, world: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }
//   @hints read-only
TSharedPtr<FJsonObject> HandleOverlapScene(const TSharedPtr<FJsonObject>& Params)
{
    FString Shape;
    if (!Params->TryGetStringField(TEXT("shape"), Shape) || Shape.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: shape (\"sphere\"|\"box\"|\"capsule\")"));
    }
    const TArray<TSharedPtr<FJsonValue>>* LocationArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("location"), LocationArr) || !LocationArr)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: location ([x, y, z])"));
    }
    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));

    // radius and half_height are read into bools tested afterwards: each shape needs only its own,
    // and a read negated inside the if would make the catalog require both for every shape.
    FCollisionShape CollisionShape;
    if (Shape.Equals(TEXT("sphere"), ESearchCase::IgnoreCase))
    {
        double Radius = 0.0;
        const bool bHasRadius = Params->TryGetNumberField(TEXT("radius"), Radius);
        if (!bHasRadius || Radius <= 0.0)
        {
            return mcp_common::MakeErrorReply(TEXT("shape=sphere requires a positive 'radius'"));
        }
        CollisionShape = FCollisionShape::MakeSphere(static_cast<float>(Radius));
    }
    else if (Shape.Equals(TEXT("box"), ESearchCase::IgnoreCase))
    {
        if (!Params->HasField(TEXT("half_extent")))
        {
            return mcp_common::MakeErrorReply(TEXT("shape=box requires 'half_extent' [x, y, z]"));
        }
        CollisionShape = FCollisionShape::MakeBox(mcp_common::ReadJsonVector(Params, TEXT("half_extent")));
    }
    else if (Shape.Equals(TEXT("capsule"), ESearchCase::IgnoreCase))
    {
        double Radius = 0.0, HalfHeight = 0.0;
        const bool bHasRadius = Params->TryGetNumberField(TEXT("radius"), Radius);
        const bool bHasHalfHeight = Params->TryGetNumberField(TEXT("half_height"), HalfHeight);
        if (!bHasRadius || Radius <= 0.0 || !bHasHalfHeight || HalfHeight <= 0.0)
        {
            return mcp_common::MakeErrorReply(TEXT("shape=capsule requires positive 'radius' and 'half_height'"));
        }
        CollisionShape = FCollisionShape::MakeCapsule(static_cast<float>(Radius), static_cast<float>(HalfHeight));
    }
    else
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown shape '%s'. Use \"sphere\", \"box\", or \"capsule\"."), *Shape));
    }

    FString ChannelName = TEXT("Visibility");
    Params->TryGetStringField(TEXT("trace_channel"), ChannelName);
    ECollisionChannel Channel = ECC_Visibility;
    FString ChannelError;
    if (!WH4ResolveCollisionChannel(ChannelName, Channel, ChannelError))
    {
        return mcp_common::MakeErrorReply(ChannelError);
    }

    FString WorldError, WorldMode;
    UWorld* World = WH4ResolveQueryWorld(Params, WorldError, WorldMode);
    if (!World)
    {
        return mcp_common::MakeErrorReply(WorldError);
    }

    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(McpOverlapScene), /*bTraceComplex=*/false);
    Physics_IgnoreListedActors(Params, World, QueryParams);

    TArray<FOverlapResult> Overlaps;
    World->OverlapMultiByChannel(Overlaps, Location, FQuat::Identity, Channel, CollisionShape, QueryParams);

    TArray<TSharedPtr<FJsonValue>> OverlapArr;
    for (const FOverlapResult& O : Overlaps)
    {
        AActor* OverlapActor = O.GetActor();
        UPrimitiveComponent* OverlapComponent = O.GetComponent();
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("actor_path"), OverlapActor ? OverlapActor->GetPathName() : FString());
        Entry->SetStringField(TEXT("actor_name"), OverlapActor ? OverlapActor->GetName() : FString());
        Entry->SetStringField(TEXT("component_name"), OverlapComponent ? OverlapComponent->GetName() : FString());
        Entry->SetNumberField(TEXT("item_index"), O.ItemIndex);
        Entry->SetBoolField(TEXT("blocking"), O.bBlockingHit);
        OverlapArr.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("overlaps"), OverlapArr);
    Result->SetNumberField(TEXT("count"), OverlapArr.Num());
    // A "count: 0" is only interpretable if you know WHICH world was queried.
    Result->SetStringField(TEXT("world"), WorldMode);
    mcp_common::AddWorldEvidence(Result, World);
    return mcp_common::MakeOkReply(Result);
}

// MCP:get_actor_bounds - Measure how BIG an actor really is: its aggregate world-space bounding box (origin, half-extent, min, max) in the EDITOR world. The size oracle for placement and footprint questions, and the honest test for "did that volume/brush actually get built" -- a spawned volume whose extent comes back (0,0,0) has no geometry no matter what the spawning tool reported. Its `components` filter is the reason to prefer it over reading a component property: "all" (default, AActor::GetActorBounds -- inflated by interaction/trigger shapes), "colliding" (only collision-enabled components) or "mesh"/"rendered" (UMeshComponent geometry ONLY, so trigger spheres/boxes are excluded and the box hugs what you actually see). Editor world only -- for a PIE-world footprint use overlap_scene/raycast_scene with a `world` selector.
//   @param actor_name [string!] - Actor name or editor label
//   @param components [string] - Which components contribute: "all" (default; everything, trigger shapes included) | "colliding" (collision-enabled only) | "mesh" | "rendered" (UMeshComponent geometry only)
//   @param include_child_actors [bool] - Include child-actor components' bounds. Defaults to false.
//   @returns { actor_name: string, components: string, valid: bool, component_count: int, origin: {x,y,z}, extent: {x,y,z}, min: {x,y,z}, max: {x,y,z} } -- valid=false with a zero extent means no contributing geometry was found
TSharedPtr<FJsonObject> HandleGetActorBounds(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }
    bool bIncludeChildActors = false;
    Params->TryGetBoolField(TEXT("include_child_actors"), bIncludeChildActors);

    FString ComponentsMode = TEXT("all");
    Params->TryGetStringField(TEXT("components"), ComponentsMode);
    ComponentsMode = ComponentsMode.ToLower();
    if (ComponentsMode != TEXT("all") && ComponentsMode != TEXT("colliding")
        && ComponentsMode != TEXT("mesh") && ComponentsMode != TEXT("rendered"))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Unknown components filter '%s' (expected all/colliding/mesh/rendered)"), *ComponentsMode));
    }

    UWorld* World = mcp_common::GetEditorWorld();
    AActor* Actor = World ? mcp_common::FindActorByName(World, ActorName) : nullptr;
    if (!Actor)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), Actor->GetName());
    Result->SetStringField(TEXT("components"), ComponentsMode);

    if (ComponentsMode == TEXT("mesh") || ComponentsMode == TEXT("rendered"))
    {
        // Aggregate ONLY UMeshComponent geometry. Interaction/trigger UShapeComponents
        // (USphereComponent/UBoxComponent/UCapsuleComponent) inflate GetActorBounds well past
        // the visible mesh; they are NOT UMeshComponents so this loop skips them by type.
        TArray<UMeshComponent*> MeshComps;
        Actor->GetComponents<UMeshComponent>(MeshComps, bIncludeChildActors);
        FBox Box(ForceInit);
        int32 Count = 0;
        for (UMeshComponent* MeshComp : MeshComps)
        {
            if (!MeshComp || !MeshComp->IsRegistered())
            {
                continue;
            }
            const FBoxSphereBounds B = MeshComp->CalcBounds(MeshComp->GetComponentTransform());
            if (B.SphereRadius <= KINDA_SMALL_NUMBER)
            {
                continue; // empty / unassigned mesh -> degenerate bounds, don't pollute the box
            }
            Box += B.GetBox();
            ++Count;
        }
        Result->SetNumberField(TEXT("component_count"), Count);
        if (!Box.IsValid)
        {
            // No mesh geometry found: report the actor location with a zero extent + valid=false
            // rather than a misleading (0,0,0)-anchored box.
            const FVector Loc = Actor->GetActorLocation();
            Result->SetBoolField(TEXT("valid"), false);
            Physics_AddBoxFields(Result, Loc, FVector::ZeroVector, Loc, Loc);
            return mcp_common::MakeOkReply(Result);
        }
        FVector Origin, Extent;
        Box.GetCenterAndExtents(Origin, Extent);
        Result->SetBoolField(TEXT("valid"), true);
        Physics_AddBoxFields(Result, Origin, Extent, Box.Min, Box.Max);
        return mcp_common::MakeOkReply(Result);
    }

    // "all" (default) or "colliding": delegate to AActor::GetActorBounds.
    //   - "all":       bOnlyCollidingComponents=false -> every component (the default).
    //   - "colliding": bOnlyCollidingComponents=true  -> only components with collision enabled.
    const bool bOnlyColliding = (ComponentsMode == TEXT("colliding"));
    FVector Origin, Extent;
    Actor->GetActorBounds(bOnlyColliding, Origin, Extent, bIncludeChildActors);

    Result->SetBoolField(TEXT("valid"), true);
    Physics_AddBoxFields(Result, Origin, Extent, Origin - Extent, Origin + Extent);
    return mcp_common::MakeOkReply(Result);
}

// MCP:get_scene_bounds - How big is the LEVEL: one world-space bounding box aggregated over every level-bounds-relevant actor in the editor world (persistent level plus any loaded streamed levels), or over an explicit subset if you pass actor_names. Use it for map extents / where-is-everything questions; use get_actor_bounds for a single actor. Note it only sees LOADED actors, so on a World-Partition map an unloaded region contributes nothing and the box reads smaller than the map.
//   @param actor_names [array] - Optional subset of actor names/labels; omit for every loaded actor in the world
//   @returns { valid: bool, origin: {x,y,z}, extent: {x,y,z}, min: {x,y,z}, max: {x,y,z}, actor_count: int }
TSharedPtr<FJsonObject> HandleGetSceneBounds(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("No valid editor world found"))) { return NoWorld; }

    FBox Bounds(ForceInit);
    int32 ActorCount = 0;

    const TArray<TSharedPtr<FJsonValue>>* NamesArr = nullptr;
    if (Params->TryGetArrayField(TEXT("actor_names"), NamesArr) && NamesArr && NamesArr->Num() > 0)
    {
        for (const TSharedPtr<FJsonValue>& V : *NamesArr)
        {
            if (AActor* Actor = mcp_common::FindActorByName(World, V->AsString()))
            {
                const FBox ActorBox = Actor->GetComponentsBoundingBox(/*bNonColliding=*/true);
                if (ActorBox.IsValid)
                {
                    Bounds += ActorBox;
                    ++ActorCount;
                }
            }
        }
    }
    else
    {
        // Same idiom as HandleGetPhysicsSettings/HandleGetCollisionInfo: walk every actor in
        // every loaded level (persistent + streamed), not just ALandscapeProxy::CalculateLevelBounds'
        // single-ULevel view.
        for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
        {
            AActor* Actor = *ActorItr;
            if (!Actor || !Actor->IsLevelBoundsRelevant())
            {
                continue;
            }
            const FBox ActorBox = Actor->GetComponentsBoundingBox(/*bNonColliding=*/true);
            if (ActorBox.IsValid)
            {
                Bounds += ActorBox;
                ++ActorCount;
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    if (!Bounds.IsValid)
    {
        Result->SetBoolField(TEXT("valid"), false);
        Result->SetNumberField(TEXT("actor_count"), 0);
        return mcp_common::MakeOkReply(Result);
    }

    FVector Origin, Extent;
    Bounds.GetCenterAndExtents(Origin, Extent);
    Result->SetBoolField(TEXT("valid"), true);
    Physics_AddBoxFields(Result, Origin, Extent, Bounds.Min, Bounds.Max);
    Result->SetNumberField(TEXT("actor_count"), ActorCount);
    return mcp_common::MakeOkReply(Result);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_impulse_to_actor"), &HandleAddImpulseToActor },
    { TEXT("add_instanced_static_mesh_component"), &HandleAddInstancedStaticMeshComponent },
    { TEXT("add_ism_instance"), &HandleAddIsmInstance },
    { TEXT("clear_ism_instances"), &HandleClearIsmInstances },
    { TEXT("get_actor_bounds"), &HandleGetActorBounds },
    { TEXT("get_collision_info"), &HandleGetCollisionInfo },
    { TEXT("get_ism_instance_info"), &HandleGetIsmInstanceInfo },
    { TEXT("get_physics_settings"), &HandleGetPhysicsSettings },
    { TEXT("get_scene_bounds"), &HandleGetSceneBounds },
    { TEXT("overlap_scene"), &HandleOverlapScene },
    { TEXT("raycast_scene"), &HandleRaycastScene },
    { TEXT("remove_ism_instance"), &HandleRemoveIsmInstance },
    { TEXT("update_ism_instance_transform"), &HandleUpdateIsmInstanceTransform },
};
} // namespace UEAgentPhysicsCommands
