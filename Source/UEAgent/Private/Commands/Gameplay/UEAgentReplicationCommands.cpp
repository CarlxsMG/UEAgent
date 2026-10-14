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
// MCP:set_actor_replicates - Toggle AActor::SetReplicates for an editor-world actor.
//   @param actor_label - Label of the actor in the editor world.
//   @param replicates - bool, new value for bReplicates.
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
// MCP:set_actor_replicate_movement - Toggle AActor::SetReplicateMovement for an editor-world actor.
//   @param actor_label - Label of the actor in the editor world.
//   @param replicate_movement - bool, new value for bReplicateMovement.
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
// MCP:set_actor_net_dormancy - Set AActor::NetDormancy for an editor-world actor.
//   @param actor_label - Label of the actor in the editor world.
//   @param dormancy - Dormancy token, e.g. "DORMANT_DormantAll", "DORMANT_Awake", "DORM_Never", "DORM_Initial".
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
// MCP:set_actor_net_update_frequency - Set AActor::NetUpdateFrequency (and optionally MinNetUpdateFrequency) for an editor-world actor.
//   @param actor_label - Label of the actor in the editor world.
//   @param frequency - Net update frequency in Hz.
//   @param min_frequency - Optional minimum net update frequency in Hz.
//   @returns { actor_label: string, frequency: float, min_frequency: float }
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
// MCP:set_actor_net_cull_distance - Set AActor::NetCullDistanceSquared for an editor-world actor (accepts the linear distance in cm).
//   @param actor_label - Label of the actor in the editor world.
//   @param distance - Net cull distance in cm (will be squared before being set).
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

// MCP:get_actor_net_update_frequency - Read AActor::NetUpdateFrequency + MinNetUpdateFrequency for an actor.
//   @param actor_name - Engine name of the actor
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

// MCP:get_actor_net_cull_distance - Read an actor's net cull distance (read-side of set_actor_net_cull_distance). It returns BOTH forms: net_cull_distance_squared, which is how UE 5.8 actually stores it (AActor::NetCullDistanceSquared), and net_cull_distance_cm, the plain linear distance -- so you never have to remember which one you are looking at.
//   @param actor_name - Engine name of the actor
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

// MCP:force_net_update - AActor::ForceNetUpdate — schedule a replication kick immediately.
//   @param actor_name - Required.
//   @returns { actor_name, forced }
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

// MCP:flush_net_dormancy - AActor::FlushNetDormancy — wake a dormant actor so it re-replicates next tick.
//   @param actor_name - Required.
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

// MCP:get_net_role - Read LocalRole + RemoteRole for an actor.
//   @param actor_name - Required.
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

// MCP:set_net_priority - AActor::NetPriority is an editable UPROPERTY (writer).
//   @param actor_name - Required.
//   @param priority - float in [0.0, 10.0] (UE range).
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

// MCP:get_net_priority - Reader for NetPriority.
//   @param actor_name - Required.
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
