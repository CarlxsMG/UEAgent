#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Editor.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"

/**
 * Multiplayer / Replication commands.
 *
 * Wraps the public AActor replication setter API:
 *  - SetReplicates / SetReplicateMovement
 *  - SetNetDormancy
 *  - SetNetUpdateFrequency / SetMinNetUpdateFrequency
 *  - SetNetCullDistanceSquared
 *
 * In 5.5+ the underlying UPROPERTYs (NetCullDistanceSquared, NetUpdateFrequency, MinNetUpdateFrequency) are
 * PRIVATE and direct property access is deprecated, so the commands use the public setters/getters that
 * BlueprintGetter/Setter routes to.
 */
namespace UEAgentReplicationCommands
{

// MCP-DOMAIN: Replication

namespace
{
	/** The editor-world actor the required actor_label names: null once OutActor holds it, otherwise the error reply. */
	TSharedPtr<FJsonObject> ResolveActorOrError(const TSharedPtr<FJsonObject>& Params, AActor*& OutActor)
	{
		FString Label;
		if (!Params->TryGetStringField(TEXT("actor_label"), Label) || Label.IsEmpty())
		{
			return mcp_common::MakeErrorReply(TEXT("actor_label is required"));
		}
		UWorld* World = mcp_common::GetEditorWorld();
		if (!World)
		{
			return mcp_common::MakeErrorReply(TEXT("No editor world available"));
		}
		OutActor = mcp_common::FindActorByLabel(World, Label);
		if (!OutActor)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Actor not found: %s"), *Label));
		}
		return nullptr;
	}

	/** The editor-world actor with that label: null once OutActor holds it, otherwise the error reply. */
	TSharedPtr<FJsonObject> Replication_FindActor(const FString& Label, AActor*& OutActor)
	{
		OutActor = mcp_common::FindActorByLabel(mcp_common::GetEditorWorld(), Label);
		if (!OutActor)
		{
			return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *Label));
		}
		return nullptr;
	}

	/** Replication_FindActor for the required actor_name (a null Params counts as missing it). */
	TSharedPtr<FJsonObject> Replication_ReadNamedActor(const TSharedPtr<FJsonObject>& Params, FString& OutName, AActor*& OutActor)
	{
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_name"), OutName) || OutName.IsEmpty())
		{
			return mcp_common::MissingParameterReply(TEXT("actor_name"));
		}
		return Replication_FindActor(OutName, OutActor);
	}

	/** Map a dormancy string to ENetDormancy. The ENetDormancy enumerators use a
	 *  DORM_ prefix (DORM_Never/DORM_Awake/DORM_DormantAll/DORM_DormantPartial/
	 *  DORM_Initial) — NOT "DORMANT_" — so we normalize to a bare core token and
	 *  match explicitly. Accepts e.g. "DORMANT_DormantAll", "DormantAll",
	 *  "DORM_DormantAll", "All", "Awake", "partial", etc. */
	bool NetDormancyFromString(const FString& In, ENetDormancy& Out)
	{
		FString K = In.ToUpper();
		K.ReplaceInline(TEXT("_"), TEXT(""));
		K.ReplaceInline(TEXT(" "), TEXT(""));
		// Strip a single leading DORMANT/DORM prefix (else-if: never both, which
		// would mangle "DORMANTDORMANTALL" -> "ANTALL").
		if (!K.RemoveFromStart(TEXT("DORMANT"))) { K.RemoveFromStart(TEXT("DORM")); }

		if (K == TEXT("NEVER"))                                  { Out = DORM_Never;          return true; }
		if (K == TEXT("AWAKE"))                                  { Out = DORM_Awake;          return true; }
		if (K == TEXT("ALL") || K == TEXT("DORMANTALL"))         { Out = DORM_DormantAll;     return true; }
		if (K == TEXT("PARTIAL") || K == TEXT("DORMANTPARTIAL")) { Out = DORM_DormantPartial; return true; }
		if (K == TEXT("INITIAL"))                                { Out = DORM_Initial;        return true; }
		return false;
	}

	/**
	 * Read a private bitfield UPROPERTY via reflection. Used for
	 * AActor::bReplicateMovement which is private in 5.7.
	 * Returns false if the property cannot be located.
	 */
	bool ReadActorBoolField(const AActor* Actor, FName PropName, bool& OutVal)
	{
		if (!Actor) { return false; }
		FProperty* Prop = Actor->GetClass()->FindPropertyByName(PropName);
		FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop);
		if (!BoolProp) { return false; }
		OutVal = BoolProp->GetPropertyValue_InContainer(Actor);
		return true;
	}
}

// ---------------------------------------------------------------------------
// set_actor_replicates
// ---------------------------------------------------------------------------
// MCP:set_actor_replicates - Set bReplicates on an editor-world actor found by label (AActor::SetReplicates, which also sets RemoteRole). Only that placed instance changes, not its class defaults; it is not marked dirty and nothing is saved. replicates echoes the request; read back with get_actor_replication_info.
//   @param actor_label - Label of the actor in the editor world.
//   @param replicates - true makes the actor replicate, false stops it (new value of bReplicates).
//   @returns { actor_label: string, replicates: bool }
TSharedPtr<FJsonObject> HandleSetActorReplicates(
    const TSharedPtr<FJsonObject>& Params)
{
	AActor* Actor = nullptr;
	if (TSharedPtr<FJsonObject> Error = ResolveActorOrError(Params, Actor)) { return Error; }

	bool bReplicates = false;
	if (!Params->TryGetBoolField(TEXT("replicates"), bReplicates))
	{
		return mcp_common::MakeErrorReply(
			TEXT("replicates (bool) is required"));
	}
	Actor->SetReplicates(bReplicates);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	Data->SetBoolField(TEXT("replicates"), bReplicates);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_actor_replicate_movement
// ---------------------------------------------------------------------------
// MCP:set_actor_replicate_movement - Set bReplicateMovement on an editor-world actor found by label (AActor::SetReplicateMovement). Only that placed instance changes, not its class defaults; it is not marked dirty and nothing is saved. replicate_movement echoes the request; read back with get_actor_replication_info.
//   @param actor_label - Label of the actor in the editor world.
//   @param replicate_movement - true replicates the movement of the actor, false stops it (new value of bReplicateMovement).
//   @returns { actor_label: string, replicate_movement: bool }
TSharedPtr<FJsonObject> HandleSetActorReplicateMovement(
    const TSharedPtr<FJsonObject>& Params)
{
	AActor* Actor = nullptr;
	if (TSharedPtr<FJsonObject> Error = ResolveActorOrError(Params, Actor)) { return Error; }

	bool bReplicateMovement = false;
	if (!Params->TryGetBoolField(TEXT("replicate_movement"), bReplicateMovement))
	{
		return mcp_common::MakeErrorReply(
			TEXT("replicate_movement (bool) is required"));
	}
	Actor->SetReplicateMovement(bReplicateMovement);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	Data->SetBoolField(TEXT("replicate_movement"), bReplicateMovement);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_actor_net_dormancy
// ---------------------------------------------------------------------------
// MCP:set_actor_net_dormancy - Set NetDormancy on an editor-world actor found by label (AActor::SetNetDormancy). Only that placed instance changes; it is not marked dirty and nothing is saved. dormancy in the reply is the parsed enum name (e.g. DORM_DormantAll), not a read-back: use get_actor_replication_info.
//   @param actor_label - Label of the actor in the editor world.
//   @param dormancy - ENetDormancy value, case-insensitive, with or without a DORM_ or DORMANT_ prefix: Never, Awake, DormantAll (or All), DormantPartial (or Partial), Initial; e.g. "DORM_DormantAll".
//   @returns { actor_label: string, dormancy: string }
TSharedPtr<FJsonObject> HandleSetActorNetDormancy(
    const TSharedPtr<FJsonObject>& Params)
{
	AActor* Actor = nullptr;
	if (TSharedPtr<FJsonObject> Error = ResolveActorOrError(Params, Actor)) { return Error; }

	FString DormancyName;
	if (!Params->TryGetStringField(TEXT("dormancy"), DormancyName) || DormancyName.IsEmpty())
	{
		return mcp_common::MakeErrorReply(
			TEXT("dormancy (string) is required (e.g. 'DORMANT_DormantAll', 'DORMANT_Awake')"));
	}
	ENetDormancy Dormancy;
	if (!NetDormancyFromString(DormancyName, Dormancy))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Invalid dormancy: %s"), *DormancyName));
	}
	Actor->SetNetDormancy(Dormancy);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	Data->SetStringField(TEXT("dormancy"), StaticEnum<ENetDormancy>()
		->GetNameStringByValue(static_cast<int64>(Dormancy)));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_actor_net_update_frequency
// ---------------------------------------------------------------------------
// MCP:set_actor_net_update_frequency - Set how often an actor placed in the editor world is considered for replication: frequency goes to NetUpdateFrequency and, when given, min_frequency to MinNetUpdateFrequency, both in updates per second, through the AActor setters. The actor is found by label (first match); only that placed instance changes, not its class defaults, with no undo step, no dirty mark and no save, and a PIE copy is never touched. Leaving out min_frequency keeps the current minimum. The reply echoes the requested numbers (min_frequency only when given), not a read-back: read them with get_actor_net_update_frequency. A missing label or frequency is an error; neither value is range-checked. Culling by distance: set_actor_net_cull_distance; priority when bandwidth is short: set_net_priority; dormancy: set_actor_net_dormancy.
//   @param actor_label - Label of the actor in the editor world (first match).
//   @param frequency - Net update frequency in Hz (updates per second), written as given; required.
//   @param min_frequency - Optional minimum net update frequency in Hz, the floor adaptive net update frequency may drop to; omitted leaves it unchanged.
//   @returns { actor_label: string, frequency: float, min_frequency: float? }
TSharedPtr<FJsonObject> HandleSetActorNetUpdateFrequency(
    const TSharedPtr<FJsonObject>& Params)
{
	AActor* Actor = nullptr;
	if (TSharedPtr<FJsonObject> Error = ResolveActorOrError(Params, Actor)) { return Error; }

	double Frequency = 0.0;
	if (!Params->TryGetNumberField(TEXT("frequency"), Frequency))
	{
		return mcp_common::MakeErrorReply(
			TEXT("frequency (Hz, float) is required"));
	}
	double MinFrequency = 0.0;
	const bool bHasMin = Params->TryGetNumberField(TEXT("min_frequency"), MinFrequency);
	Actor->SetNetUpdateFrequency(static_cast<float>(Frequency));
	if (bHasMin)
	{
		Actor->SetMinNetUpdateFrequency(static_cast<float>(MinFrequency));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	Data->SetNumberField(TEXT("frequency"), Frequency);
	if (bHasMin) { Data->SetNumberField(TEXT("min_frequency"), MinFrequency); }
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_actor_net_cull_distance
// ---------------------------------------------------------------------------
// MCP:set_actor_net_cull_distance - Set the net cull distance of an actor placed in the editor world, found by label (first match): distance is the linear radius in cm, squared before AActor::SetNetCullDistanceSquared stores it (15000, the engine default, stores 225000000). Past that distance from a client's viewer the actor is no longer relevant for replication to that client, unless it is always relevant. Only that placed instance changes, not its class defaults, with no undo step, no dirty mark and no save, and a PIE copy is never touched. distance echoes the request, not a read-back: get_actor_net_cull_distance returns both stored forms. A missing label or distance is an error; no range check is made. Update rate: set_actor_net_update_frequency; every replication setting at once: get_actor_replication_info.
//   @param actor_label - Label of the actor in the editor world (first match).
//   @param distance - Net cull distance in cm (will be squared before being set); a negative value squares to the same distance.
//   @returns { actor_label: string, distance: float }
TSharedPtr<FJsonObject> HandleSetActorNetCullDistance(
    const TSharedPtr<FJsonObject>& Params)
{
	AActor* Actor = nullptr;
	if (TSharedPtr<FJsonObject> Error = ResolveActorOrError(Params, Actor)) { return Error; }

	double Distance = 0.0;
	if (!Params->TryGetNumberField(TEXT("distance"), Distance))
	{
		return mcp_common::MakeErrorReply(
			TEXT("distance (cm) is required"));
	}
	// The setter takes the squared distance. We accept the linear distance
	// from the LLM and square it for them — that matches the legacy
	// NetCullDistance UPROPERTY semantics that designers are used to.
	Actor->SetNetCullDistanceSquared(static_cast<float>(Distance * Distance));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	Data->SetNumberField(TEXT("distance"), Distance);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_actor_replication_info
// ---------------------------------------------------------------------------
// MCP:get_actor_replication_info - Read replication settings for an editor-world actor (replicates, replicate_movement via reflection, frequencies, dormancy, role).
//   @param actor_label - Label of the actor in the editor world.
//   @returns { actor_label: string, class_name: string, replicates: bool, replicate_movement: bool, net_update_frequency: float, min_net_update_frequency: float, net_cull_distance_squared: float, net_dormancy: string, role: string }
TSharedPtr<FJsonObject> HandleGetActorReplicationInfo(
    const TSharedPtr<FJsonObject>& Params)
{
	AActor* Actor = nullptr;
	if (TSharedPtr<FJsonObject> Error = ResolveActorOrError(Params, Actor)) { return Error; }

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	Data->SetStringField(TEXT("class_name"), Actor->GetClass()->GetName());
	Data->SetBoolField(TEXT("replicates"), Actor->GetIsReplicated());
	// AActor::bReplicateMovement is private in UE 5.7. Read it via reflection.
	// The macro GET_MEMBER_NAME_CHECKED needs C++ access to the field, so we
	// hard-code the FName string here (matches the UPROPERTY name in source).
	bool bReplicateMovement = false;
	ReadActorBoolField(Actor, FName(TEXT("bReplicateMovement")), bReplicateMovement);
	Data->SetBoolField(TEXT("replicate_movement"), bReplicateMovement);
	// 5.5+ uses GetNetUpdateFrequency / GetNetCullDistanceSquared via BlueprintGetter.
	Data->SetNumberField(TEXT("net_update_frequency"),
		Actor->GetNetUpdateFrequency());
	Data->SetNumberField(TEXT("min_net_update_frequency"),
		Actor->GetMinNetUpdateFrequency());
	Data->SetNumberField(TEXT("net_cull_distance_squared"),
		Actor->GetNetCullDistanceSquared());
	Data->SetStringField(TEXT("net_dormancy"),
		StaticEnum<ENetDormancy>()->GetNameStringByValue(
			static_cast<int64>(Actor->NetDormancy)));
	Data->SetStringField(TEXT("role"), StaticEnum<ENetRole>()
		? StaticEnum<ENetRole>()->GetNameStringByValue(static_cast<int64>(Actor->GetLocalRole()))
		: FString());
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// Read-side inverses of net_update_frequency + net_cull_distance.
// ---------------------------------------------------------------------------

// MCP:get_actor_net_update_frequency - Read NetUpdateFrequency and MinNetUpdateFrequency (Hz) of an editor-world actor found by label. Read-only; reads the placed instance, not a PIE copy. The reply actor_name is the engine object name. Writer: set_actor_net_update_frequency.
//   @param actor_name - Label of the actor in the editor world (matched against labels, not engine object names, despite the parameter name); first match.
//   @returns { actor_name, net_update_frequency, min_net_update_frequency }

TSharedPtr<FJsonObject> HandleGetActorNetUpdateFrequency(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }
	AActor* Actor = nullptr;
	if (TSharedPtr<FJsonObject> NotFound = Replication_FindActor(ActorName, Actor)) { return NotFound; }
	TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetStringField(TEXT("actor_name"), Actor->GetName());
	Resp->SetNumberField(TEXT("net_update_frequency"), Actor->GetNetUpdateFrequency());
	Resp->SetNumberField(TEXT("min_net_update_frequency"), Actor->GetMinNetUpdateFrequency());
	return mcp_common::MakeOkReply(Resp);
}

// MCP:get_actor_net_cull_distance - Read an actor's net cull distance (read-side of set_actor_net_cull_distance). It returns BOTH forms: net_cull_distance_squared, which is how UE 5.8 actually stores it (AActor::NetCullDistanceSquared), and net_cull_distance_cm, the plain linear distance -- so you never have to remember which one you are looking at. Read-only; the actor is found by label in the editor world, as the other replication tools find it, and the placed instance is read, not a PIE copy. The reply actor_name is the engine object name. An unknown label is an error.
//   @param actor_name - Label of the actor in the editor world (matched against labels, not engine object names, despite the parameter name); first match.
//   @returns { actor_name, net_cull_distance_squared, net_cull_distance_cm }

TSharedPtr<FJsonObject> HandleGetActorNetCullDistance(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }
	AActor* Actor = nullptr;
	if (TSharedPtr<FJsonObject> NotFound = Replication_FindActor(ActorName, Actor)) { return NotFound; }
	TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetStringField(TEXT("actor_name"), Actor->GetName());
	Resp->SetNumberField(TEXT("net_cull_distance_squared"), Actor->GetNetCullDistanceSquared());
	Resp->SetNumberField(TEXT("net_cull_distance_cm"), FMath::Sqrt(Actor->GetNetCullDistanceSquared()));
	return mcp_common::MakeOkReply(Resp);
}

// ============================================================================
// Force / flush / role / priority primitives.
// ============================================================================

// MCP:force_net_update - Call AActor::ForceNetUpdate on an actor placed in the editor world, found by label (first match). The engine call asks the world's net drivers to replicate the actor on their next update, but the editor world has no net driver, so nothing is sent, no PIE server or client copy is reached and no state changes: nothing is marked dirty, saved or undoable, and repeating it is harmless. forced is always true and proves only that the call was made. A missing or unknown label is an error. To change how often the actor replicates use set_actor_net_update_frequency; to wake a dormant actor, flush_net_dormancy or set_actor_net_dormancy; to read its settings, get_actor_replication_info.
//   @param actor_name - Label of the actor in the editor world (matched against labels, not object names); first match.
//   @returns { actor_name, forced }
//   @hints read-only
TSharedPtr<FJsonObject> HandleForceNetUpdate(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    AActor* Actor = nullptr;
    if (TSharedPtr<FJsonObject> Error = Replication_ReadNamedActor(Params, ActorName, Actor)) { return Error; }
    Actor->ForceNetUpdate();
    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_name"), ActorName);
    R->SetBoolField(TEXT("forced"), true);
    return mcp_common::MakeOkReply(R);
}

// MCP:flush_net_dormancy - Call AActor::FlushNetDormancy, which makes a dormant replicated actor send its state again, on an editor-world actor found by label. The editor world has no net driver, so nothing is sent and PIE is never reached, but DORM_Initial still becomes DORM_DormantAll. flushed is always true.
//   @param actor_name - Label of the actor in the editor world (matched against labels, not object names); first match.
//   @returns { actor_name, flushed }
TSharedPtr<FJsonObject> HandleFlushNetDormancy(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    AActor* Actor = nullptr;
    if (TSharedPtr<FJsonObject> Error = Replication_ReadNamedActor(Params, ActorName, Actor)) { return Error; }
    Actor->FlushNetDormancy();
    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_name"), ActorName);
    R->SetBoolField(TEXT("flushed"), true);
    return mcp_common::MakeOkReply(R);
}

// MCP:get_net_role - Read LocalRole and RemoteRole (number and ENetRole name) of an editor-world actor found by label. Read-only; it reads the placed instance, not a PIE server or client copy, so it shows the authoring-time roles.
//   @param actor_name - Label of the actor in the editor world (matched against labels, not object names); first match.
//   @returns { actor_name, local_role, local_role_name, remote_role, remote_role_name }
TSharedPtr<FJsonObject> HandleGetNetRole(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    AActor* Actor = nullptr;
    if (TSharedPtr<FJsonObject> Error = Replication_ReadNamedActor(Params, ActorName, Actor)) { return Error; }
    const ENetRole Local = Actor->GetLocalRole();
    const ENetRole Remote = Actor->GetRemoteRole();
    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_name"), ActorName);
    R->SetNumberField(TEXT("local_role"), static_cast<int32>(Local));
    R->SetStringField(TEXT("local_role_name"),
        StaticEnum<ENetRole>() ? StaticEnum<ENetRole>()->GetNameStringByValue(static_cast<int64>(Local)) : FString());
    R->SetNumberField(TEXT("remote_role"), static_cast<int32>(Remote));
    R->SetStringField(TEXT("remote_role_name"),
        StaticEnum<ENetRole>() ? StaticEnum<ENetRole>()->GetNameStringByValue(static_cast<int64>(Remote)) : FString());
    return mcp_common::MakeOkReply(R);
}

// MCP:set_net_priority - Write NetPriority, the relative replication priority when bandwidth is short, on an editor-world actor found by label. Only that placed instance changes; it is not marked dirty or saved. priority is read back after the write. Engine defaults: 1.0 for actors, 3.0 for pawns.
//   @param actor_name - Label of the actor in the editor world (matched against labels, not object names); first match.
//   @param priority - New NetPriority as a float, written as given with no clamping; higher makes the actor more likely to replicate when bandwidth is saturated.
//   @returns { actor_name, priority }
TSharedPtr<FJsonObject> HandleSetNetPriority(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    double Prio = 0.0;
    if (!Params.IsValid() ||
        !Params->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty() ||
        !Params->TryGetNumberField(TEXT("priority"), Prio))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required: actor_name, priority"));
    }
    AActor* Actor = nullptr;
    if (TSharedPtr<FJsonObject> NotFound = Replication_FindActor(ActorName, Actor)) { return NotFound; }
    Actor->NetPriority = static_cast<float>(Prio);
    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_name"), ActorName);
    R->SetNumberField(TEXT("priority"), Actor->NetPriority);
    return mcp_common::MakeOkReply(R);
}

// MCP:get_net_priority - Read NetPriority, the relative replication priority when bandwidth is short, of an editor-world actor found by label. Read-only; reads the placed instance, not a PIE copy. Engine defaults: 1.0 for actors, 3.0 for pawns. Writer: set_net_priority.
//   @param actor_name - Label of the actor in the editor world (matched against labels, not object names); first match.
//   @returns { actor_name, priority }
TSharedPtr<FJsonObject> HandleGetNetPriority(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    AActor* Actor = nullptr;
    if (TSharedPtr<FJsonObject> Error = Replication_ReadNamedActor(Params, ActorName, Actor)) { return Error; }
    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_name"), ActorName);
    R->SetNumberField(TEXT("priority"), Actor->NetPriority);
    return mcp_common::MakeOkReply(R);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("flush_net_dormancy"), &HandleFlushNetDormancy },
	{ TEXT("force_net_update"), &HandleForceNetUpdate },
	{ TEXT("get_actor_net_cull_distance"), &HandleGetActorNetCullDistance },
	{ TEXT("get_actor_net_update_frequency"), &HandleGetActorNetUpdateFrequency },
	{ TEXT("get_actor_replication_info"), &HandleGetActorReplicationInfo },
	{ TEXT("get_net_priority"), &HandleGetNetPriority },
	{ TEXT("get_net_role"), &HandleGetNetRole },
	{ TEXT("set_actor_net_cull_distance"), &HandleSetActorNetCullDistance },
	{ TEXT("set_actor_net_dormancy"), &HandleSetActorNetDormancy },
	{ TEXT("set_actor_net_update_frequency"), &HandleSetActorNetUpdateFrequency },
	{ TEXT("set_actor_replicate_movement"), &HandleSetActorReplicateMovement },
	{ TEXT("set_actor_replicates"), &HandleSetActorReplicates },
	{ TEXT("set_net_priority"), &HandleSetNetPriority },
};
} // namespace UEAgentReplicationCommands
