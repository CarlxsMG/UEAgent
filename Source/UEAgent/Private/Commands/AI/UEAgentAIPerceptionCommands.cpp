// Copyright (c) 2026 CarlxsMG.

// Native wrapper around UAIPerceptionStimuliSourceComponent (UE 5.8 source:
// AIModule/Classes/Perception/AIPerceptionStimuliSourceComponent.h). The component is the
// canonical engine way to make an actor perceivable by an AI Perception system.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Gameplay/GameplayActorLookup.h"

#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
// Stimuli source component (AIModule — declared plugin dependency).
#include "Perception/AIPerceptionStimuliSourceComponent.h"
// AI Sight sense class (we register for "Sight" by default; "Hearing"/"Damage" available).
#include "Perception/AISense_Sight.h"
#include "Perception/AISense_Hearing.h"
#include "Perception/AISense_Damage.h"
// Sense config (per-sense activation / expiration).
#include "Perception/AIPerceptionTypes.h"
// AI controller Blueprint authoring.
#include "AIController.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "UObject/Package.h"
// Runtime AI perception introspection and configuration.
#include "GameFramework/Pawn.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig.h"
#include "Perception/AISenseConfig_Sight.h"
// Native FGenericTeamId support.
#include "GenericTeamAgentInterface.h"

#define LOCTEXT_NAMESPACE "UEAgentAIPerception"

/**
 * AI perception commands on the native engine types: stimuli sources, AIController Blueprints, runtime
 * sight configuration and perceived actors, and FGenericTeamId teams and attitudes.
 */
namespace UEAgentAIPerceptionCommands
{

// MCP-DOMAIN: AI

namespace
{
    /** Resolve an AISense subclass from a JSON name; defaults to UAISense_Sight. */
    TSubclassOf<UAISense> ResolveSenseClass(const FString& Name)
    {
        if (Name.Equals(TEXT("Hearing"), ESearchCase::IgnoreCase))
            return UAISense_Hearing::StaticClass();
        if (Name.Equals(TEXT("Damage"), ESearchCase::IgnoreCase))
            return UAISense_Damage::StaticClass();
        // default: Sight (also covers "Team", "Touch" via UAISense_* siblings)
        return UAISense_Sight::StaticClass();
    }
}

// MCP:add_ai_stimuli_source - Adds a UAIPerceptionStimuliSourceComponent to an actor and registers it as a source for the supplied AISense (default Sight). Idempotent: re-run returns the existing component.
//   @param actor_name - Display label or internal name of the target editor-world actor (required)
//   @param sense - "Sight" | "Hearing" | "Damage" (default "Sight")
//   @returns { actor_name: string, component_name: string, sense: string, registered: bool, already_existed: bool }
TSharedPtr<FJsonObject> HandleAddAiStimuliSource(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }

    AActor* Actor = mcp_common::FindActorByName(World, ActorName);
    if (!Actor)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor '%s' not found"), *ActorName));

    FString SenseName = TEXT("Sight");
    Params->TryGetStringField(TEXT("sense"), SenseName);

    // Idempotency: re-use the first existing stimuli source on the actor if one is present.
    UAIPerceptionStimuliSourceComponent* Existing = Actor->FindComponentByClass<UAIPerceptionStimuliSourceComponent>();
    if (Existing)
    {
        const TSubclassOf<UAISense> SenseClass = ResolveSenseClass(SenseName);
        Existing->RegisterForSense(SenseClass);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("actor_name"), ActorName);
        Result->SetStringField(TEXT("component_name"), Existing->GetName());
        Result->SetStringField(TEXT("sense"), SenseName);
        Result->SetBoolField(TEXT("registered"), true);
        Result->SetBoolField(TEXT("already_existed"), true);
        return mcp_common::MakeOkReply(Result);
    }

    const FScopedTransaction Transaction(LOCTEXT("AddAiStimuliSource", "MCP: Add AI Stimuli Source"));
    Actor->Modify();

    UAIPerceptionStimuliSourceComponent* Comp = NewObject<UAIPerceptionStimuliSourceComponent>(Actor);
    if (!Comp)
        return mcp_common::MakeErrorReply(TEXT("Failed to create UAIPerceptionStimuliSourceComponent"));

    Comp->CreationMethod = EComponentCreationMethod::Instance;
    const TSubclassOf<UAISense> SenseClass = ResolveSenseClass(SenseName);
    Comp->RegisterForSense(SenseClass);

    Comp->RegisterComponent();
    Actor->AddInstanceComponent(Comp);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetStringField(TEXT("component_name"), Comp->GetName());
    Result->SetStringField(TEXT("sense"), SenseName);
    Result->SetBoolField(TEXT("registered"), true);
    Result->SetBoolField(TEXT("already_existed"), false);
    return mcp_common::MakeOkReply(Result);
}

// create_ai_controller_blueprint
// ---------------------------------------------------------------------------- //
// MCP:create_ai_controller_blueprint - Create a Blueprint asset parented to AAIController, compile and save it.  Idempotent: if the asset already exists it is loaded and returned.
//   @param name - Asset name (e.g. BP_EnemyAIController)
//   @param package_path - Target /Game package path (optional, default "/Game/Blueprints")
//   @returns { name: string, asset_path: string, parent_class: string, generated_class: string, is_ai_controller: bool, already_existed: bool, created: bool }

TSharedPtr<FJsonObject> HandleCreateAIControllerBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString PackagePath = TEXT("/Game/Blueprints");
    Params->TryGetStringField(TEXT("package_path"), PackagePath);
    PackagePath = mcp_common::NormalizeFolder(PackagePath, TEXT("/Game/Blueprints"));

    bool bAlreadyExisted = false;
    FString Error;
    UBlueprint* Blueprint = mcp_common::CreateOrLoadBlueprint(AAIController::StaticClass(), PackagePath, Name, bAlreadyExisted, Error);
    if (!Blueprint)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    const bool bIsAIController = Blueprint->GeneratedClass && Blueprint->GeneratedClass->IsChildOf(AAIController::StaticClass());

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), Name);
    Result->SetStringField(TEXT("asset_path"), FString::Printf(TEXT("%s/%s"), *PackagePath, *Name));
    Result->SetStringField(TEXT("parent_class"), TEXT("AIController"));
    Result->SetStringField(TEXT("generated_class"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetName() : FString());
    Result->SetBoolField(TEXT("is_ai_controller"), bIsAIController);
    Result->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Result->SetBoolField(TEXT("created"), true);
    return mcp_common::MakeOkReply(Result);
}

// Runtime AI perception introspection and configuration.

// MCP:set_ai_sight_radius - Set SightRadius and LoseSightRadius (default sight_radius times 1.5) on the sight config of the AIPerceptionComponent of an AI-possessed pawn or an AIController. The actor is looked up in the editor world, not PIE, so an unpossessed pawn is an error. Returns sight_radius, lose_sight_radius and sight_sense_count; 0 means no sight config, nothing changed.
//   @renamed-from configure_ai_sight_radius
//   @param actor_name - Object name or editor label of the pawn or AIController in the editor world; the first match is used.
//   @param sight_radius - New SightRadius in cm, greater than 0.
//   @param lose_sight_radius - New LoseSightRadius in cm, the distance at which an already seen target is lost, 0 or more; default sight_radius times 1.5.
//   @returns { actor_name, sight_radius, lose_sight_radius, sight_sense_count }
TSharedPtr<FJsonObject> HandleConfigureAISightRadius(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    double Sight = 0.0, Lose = -1.0;
    if (!Params.IsValid() ||
        !Params->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty() ||
        !Params->TryGetNumberField(TEXT("sight_radius"), Sight) || Sight <= 0.0)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required: actor_name, sight_radius (>0)"));
    }
    if (Params->TryGetNumberField(TEXT("lose_sight_radius"), Lose) && Lose < 0.0)
    {
        return mcp_common::MakeErrorReply(TEXT("lose_sight_radius must be 0 or more"));
    }
    AActor* Actor = mcp_common::FindActorByName(mcp_common::GetEditorWorld(), ActorName);
    if (!Actor)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }
    AAIController* AICon = GameplayActorLookup::FindAIController(Actor);
    if (!AICon)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor '%s' is not AI-possessed -- no AIPerceptionComponent available headless."), *ActorName));
    }
    UAIPerceptionComponent* PercComp = AICon->GetPerceptionComponent();
    if (!PercComp)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor '%s' has no UAIPerceptionComponent."), *ActorName));
    }
    float SightRadius = static_cast<float>(Sight);
    float LoseRadius = static_cast<float>((Lose < 0.0) ? (Sight * 1.5) : Lose);
    // Mutate the Sight config in-place via the typed accessor; the runtime perception
    // system picks up the change on the next tick (headless-safe path).
    int32 SightSenseCount = 0;
    if (UAISenseConfig_Sight* SightCfg = PercComp->GetSenseConfig<UAISenseConfig_Sight>())
    {
        ++SightSenseCount;
        SightCfg->SightRadius = SightRadius;
        SightCfg->LoseSightRadius = LoseRadius;
        // Report what the config holds, not the request.
        SightRadius = SightCfg->SightRadius;
        LoseRadius = SightCfg->LoseSightRadius;
    }
    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_name"), ActorName);
    R->SetNumberField(TEXT("sight_radius"), SightRadius);
    R->SetNumberField(TEXT("lose_sight_radius"), LoseRadius);
    R->SetNumberField(TEXT("sight_sense_count"), SightSenseCount);
    return mcp_common::MakeOkReply(R);
}

// MCP:get_ai_perceived_actors - List the actors that the AIPerceptionComponent of an AI-possessed pawn, or of an AIController, currently perceives by any sense, without changing anything. The actor is looked up in the editor world, not PIE, so an unpossessed pawn is an error. Returns count and perceived [{actor_name}]; no last-sensed time is included.
//   @param actor_name - Object name or editor label of the pawn or AIController in the editor world; the first match is used.
//   @returns { actor_name, perceived: [{actor_name}], count }
TSharedPtr<FJsonObject> HandleGetAIPerceivedActors(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'actor_name'"));
    }
    AActor* Actor = mcp_common::FindActorByName(mcp_common::GetEditorWorld(), ActorName);
    if (!Actor)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }
    AAIController* AICon = GameplayActorLookup::FindAIController(Actor);
    if (!AICon)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor '%s' has no AAIController."), *ActorName));
    }
    UAIPerceptionComponent* PercComp = AICon->GetPerceptionComponent();
    if (!PercComp)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("No UAIPerceptionComponent on '%s'."), *ActorName));
    }
    TArray<AActor*> Perceived;
    PercComp->GetCurrentlyPerceivedActors(nullptr, Perceived);  // nullptr = any sense
    TArray<TSharedPtr<FJsonValue>> Arr;
    for (AActor* A : Perceived)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("actor_name"), A->GetName());
        Arr.Add(MakeShared<FJsonValueObject>(O));
    }
    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_name"), ActorName);
    R->SetArrayField(TEXT("perceived"), Arr);
    R->SetNumberField(TEXT("count"), Perceived.Num());
    return mcp_common::MakeOkReply(R);
}

// ============================================================================
// Native FGenericTeamId support -- UE 5.8's own team/affiliation system, the one
// AI Perception (Sight/Hearing/Touch DetectionByAffiliation) actually reads; the
// plugin keeps no team table of its own. Only
// AAIController implements IGenericTeamAgentInterface in vanilla 5.8 — a Pawn
// resolves its team via its possessing AIController.
// ============================================================================

namespace
{
    // Symmetric attitude table backing the MCP-installed FGenericTeamId solver.
    // Process-wide static: FGenericTeamId::AttitudeSolverImpl is itself a class
    // static (not per-world/per-actor state), so this mirrors that lifetime.
    TMap<uint16, ETeamAttitude::Type> GMcpNativeTeamAttitudeTable;
    bool GMcpNativeTeamAttitudeSolverInstalled = false;

    uint16 McpTeamPairKey(uint8 A, uint8 B)
    {
        const uint8 Lo = FMath::Min(A, B);
        const uint8 Hi = FMath::Max(A, B);
        return (static_cast<uint16>(Lo) << 8) | static_cast<uint16>(Hi);
    }

    ETeamAttitude::Type McpNativeTeamAttitudeSolver(FGenericTeamId TeamA, FGenericTeamId TeamB)
    {
        if (const ETeamAttitude::Type* Found = GMcpNativeTeamAttitudeTable.Find(McpTeamPairKey(TeamA.GetId(), TeamB.GetId())))
        {
            return *Found;
        }
        // Engine default (FGenericTeamId's own default solver): same team -> Friendly, else Hostile.
        return TeamA.GetId() == TeamB.GetId() ? ETeamAttitude::Friendly : ETeamAttitude::Hostile;
    }

    /** Resolve the IGenericTeamAgentInterface for an actor, falling back to its Controller for a Pawn. */
    IGenericTeamAgentInterface* ResolveTeamAgent(AActor* Actor, FString& OutResolvedOn, AAIController** OutController = nullptr)
    {
        if (IGenericTeamAgentInterface* Direct = Cast<IGenericTeamAgentInterface>(Actor))
        {
            OutResolvedOn = TEXT("actor");
            return Direct;
        }
        if (APawn* P = Cast<APawn>(Actor))
        {
            if (AAIController* AICon = Cast<AAIController>(P->GetController()))
            {
                if (IGenericTeamAgentInterface* ViaController = Cast<IGenericTeamAgentInterface>(AICon))
                {
                    OutResolvedOn = TEXT("controller");
                    if (OutController) { *OutController = AICon; }
                    return ViaController;
                }
            }
        }
        OutResolvedOn = TEXT("none");
        return nullptr;
    }
}

// MCP:set_actor_team - Set the native team id (FGenericTeamId) of an actor in the editor world through IGenericTeamAgentInterface, falling back to its Controller when the actor does not implement it (an ACharacter possessed by an AAIController, the only implementer in vanilla UE 5.8), then refresh that agent's AI Perception listeners so Sight, Hearing and Touch affiliation filtering sees the new team at once. The previous team is overwritten in memory only: no undo step, nothing dirtied or saved, and on an AAIController it is gone when the level reloads. The same id again gives the same result. Read it with get_actor_team; set_native_team_attitude decides whether two teams are friendly, neutral or hostile. An unknown actor, a team_id outside 0-254, or no team interface on the actor or its controller is an error and nothing changes.
//   @param actor_name - Object name or label of the actor in the editor world (a pawn, or the AIController itself), e.g. 'BP_Guard_C_0'.
//   @param team_id - Team number, an integer in [0, 254], e.g. 1; 255 is the engine-reserved NoTeam id and is refused.
//   @returns { actor_name, team_id, applied, resolved_on: "actor"|"controller", perception_listeners_refreshed }
TSharedPtr<FJsonObject> HandleSetActorTeam(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    double TeamIdD = 0.0;
    if (!Params.IsValid() ||
        !Params->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty() ||
        !Params->TryGetNumberField(TEXT("team_id"), TeamIdD))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required: actor_name, team_id"));
    }
    const int32 TeamIdInt = static_cast<int32>(TeamIdD);
    if (TeamIdInt < 0 || TeamIdInt > 254)
    {
        return mcp_common::MakeErrorReply(TEXT("'team_id' must be in [0, 254] (255 is the reserved NoTeam id)"));
    }
    AActor* Actor = mcp_common::FindActorByName(mcp_common::GetEditorWorld(), ActorName);
    if (!Actor)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    FString ResolvedOn;
    AAIController* OwningController = nullptr;
    IGenericTeamAgentInterface* TeamAgent = ResolveTeamAgent(Actor, ResolvedOn, &OwningController);
    if (!TeamAgent)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Actor '%s' does not implement IGenericTeamAgentInterface, and neither does its Controller (only AAIController does in vanilla UE 5.8 -- possess it with an AI Controller first)."), *ActorName));
    }

    const FGenericTeamId NewTeamId(static_cast<uint8>(TeamIdInt));
    TeamAgent->SetGenericTeamId(NewTeamId);

    // Refresh any perception listeners so Sight/Hearing/Touch affiliation filtering
    // (cached at listener-update time, per the AISense_Hearing/Touch delivery path)
    // picks up the new team without waiting for an unrelated config change.
    int32 RefreshedListeners = 0;
    AActor* PerceptionOwner = OwningController ? static_cast<AActor*>(OwningController) : Actor;
    for (UActorComponent* Comp : PerceptionOwner->GetComponents())
    {
        if (UAIPerceptionComponent* PercComp = Cast<UAIPerceptionComponent>(Comp))
        {
            PercComp->RequestStimuliListenerUpdate();
            ++RefreshedListeners;
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetNumberField(TEXT("team_id"), NewTeamId.GetId());
    Result->SetBoolField(TEXT("applied"), true);
    Result->SetStringField(TEXT("resolved_on"), ResolvedOn);
    Result->SetNumberField(TEXT("perception_listeners_refreshed"), RefreshedListeners);
    return mcp_common::MakeOkReply(Result);
}

// MCP:get_actor_team - Reads the native FGenericTeamId assigned to an actor (or its Controller, for a Pawn possessed by an AAIController). Returns the engine-reserved NoTeam id (255) if neither the actor nor its Controller implements IGenericTeamAgentInterface.
//   @param actor_name - Display label or internal name (required)
//   @returns { actor_name, team_id, resolved_on: "actor"|"controller"|"none", is_no_team }
TSharedPtr<FJsonObject> HandleGetActorTeam(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'actor_name'"));
    }
    AActor* Actor = mcp_common::FindActorByName(mcp_common::GetEditorWorld(), ActorName);
    if (!Actor)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    FString ResolvedOn;
    IGenericTeamAgentInterface* TeamAgent = ResolveTeamAgent(Actor, ResolvedOn);
    const FGenericTeamId TeamId = TeamAgent ? TeamAgent->GetGenericTeamId() : FGenericTeamId::NoTeam;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetNumberField(TEXT("team_id"), TeamId.GetId());
    Result->SetStringField(TEXT("resolved_on"), ResolvedOn);
    Result->SetBoolField(TEXT("is_no_team"), TeamId.GetId() == FGenericTeamId::NoTeam.GetId());
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_native_team_attitude - Installs (once per process) a custom FGenericTeamId::AttitudeSolver backed by an MCP-owned symmetric table, then records the pairwise attitude for team_a/team_b. Pairs not explicitly set fall back to the engine's own default (same team -> Friendly, else Hostile). This drives the engine's own AI Perception affiliation filtering (Sight/Hearing/Touch DetectionByAffiliation), which reads FGenericTeamId::GetAttitude directly.
//   @param team_a - Required int in [0, 255].
//   @param team_b - Required int in [0, 255].
//   @param attitude - Required: "Friendly"|"Neutral"|"Hostile".
//   @returns { team_a, team_b, attitude, resolved_attitude, solver_installed }
TSharedPtr<FJsonObject> HandleSetNativeTeamAttitude(const TSharedPtr<FJsonObject>& Params)
{
    double TeamAD = 0.0, TeamBD = 0.0;
    FString AttitudeStr;
    if (!Params.IsValid() ||
        !Params->TryGetNumberField(TEXT("team_a"), TeamAD) ||
        !Params->TryGetNumberField(TEXT("team_b"), TeamBD) ||
        !Params->TryGetStringField(TEXT("attitude"), AttitudeStr) || AttitudeStr.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required: team_a, team_b, attitude"));
    }
    const int32 TeamAInt = static_cast<int32>(TeamAD);
    const int32 TeamBInt = static_cast<int32>(TeamBD);
    if (TeamAInt < 0 || TeamAInt > 255 || TeamBInt < 0 || TeamBInt > 255)
    {
        return mcp_common::MakeErrorReply(TEXT("'team_a'/'team_b' must be in [0, 255]"));
    }

    ETeamAttitude::Type Parsed = ETeamAttitude::Neutral;
    bool bMatched = false;
    if (const UEnum* EnumPtr = StaticEnum<ETeamAttitude::Type>())
    {
        const FString Trimmed = AttitudeStr.TrimStartAndEnd();
        for (int32 i = 0; i < EnumPtr->NumEnums() - 1; ++i)
        {
            if (EnumPtr->GetNameStringByIndex(i).Equals(Trimmed, ESearchCase::IgnoreCase))
            {
                Parsed = static_cast<ETeamAttitude::Type>(EnumPtr->GetValueByIndex(i));
                bMatched = true;
                break;
            }
        }
    }
    if (!bMatched)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown attitude '%s' (valid: Friendly|Neutral|Hostile)"), *AttitudeStr));
    }

    if (!GMcpNativeTeamAttitudeSolverInstalled)
    {
        FGenericTeamId::SetAttitudeSolver(FGenericTeamId::FAttitudeSolverFunction(&McpNativeTeamAttitudeSolver));
        GMcpNativeTeamAttitudeSolverInstalled = true;
    }
    GMcpNativeTeamAttitudeTable.Add(McpTeamPairKey(static_cast<uint8>(TeamAInt), static_cast<uint8>(TeamBInt)), Parsed);

    // Round-trip through the REAL engine solver pipeline (FGenericTeamId::GetAttitude),
    // not just an echo of the input, to prove the installed solver is live and that AI
    // Perception will observe the same value.
    const ETeamAttitude::Type Resolved = FGenericTeamId::GetAttitude(
        FGenericTeamId(static_cast<uint8>(TeamAInt)), FGenericTeamId(static_cast<uint8>(TeamBInt)));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("team_a"), TeamAInt);
    Result->SetNumberField(TEXT("team_b"), TeamBInt);
    Result->SetStringField(TEXT("attitude"), AttitudeStr);
    Result->SetNumberField(TEXT("resolved_attitude"), static_cast<int32>(Resolved));
    Result->SetBoolField(TEXT("solver_installed"), GMcpNativeTeamAttitudeSolverInstalled);
    return mcp_common::MakeOkReply(Result);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_ai_stimuli_source"), &HandleAddAiStimuliSource },
    { TEXT("set_ai_sight_radius"), &HandleConfigureAISightRadius },
    { TEXT("create_ai_controller_blueprint"), &HandleCreateAIControllerBlueprint },
    { TEXT("get_actor_team"), &HandleGetActorTeam },
    { TEXT("get_ai_perceived_actors"), &HandleGetAIPerceivedActors },
    { TEXT("set_actor_team"), &HandleSetActorTeam },
    { TEXT("set_native_team_attitude"), &HandleSetNativeTeamAttitude },
};
} // namespace UEAgentAIPerceptionCommands
