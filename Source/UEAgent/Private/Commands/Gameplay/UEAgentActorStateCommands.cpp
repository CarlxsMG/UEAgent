//
// Actor-state persistence commands (serialize/restore_actor_state).
//
// Domain: SaveGame, not Gameplay -- these commands are generic persistence
// (the same domain UEAgentSaveGameCommands uses; domain is per family, and
// several families may share one).
#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "GameFramework/Actor.h"
#include "JsonObjectConverter.h"
#include "Dom/JsonObject.h"
#include "Kismet/GameplayStatics.h"
#include "MCPSaveGame.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UEAgentActorStateCommands
{

// MCP-DOMAIN: SaveGame

// ============================================================================
// Actor state persistence (transform + property set).
//
// Persists through UGameplayStatics::SaveGameToSlot/LoadGameFromSlot, the same
// mechanism as the SaveGame domain, rather than raw JSON files: one route for
// "persist a named blob" (UMCPSaveGame.Payload already
// covers "an arbitrary string blob under a name"). It also needs no live
// UGameInstanceSubsystem, so these commands work outside PIE.
//
// Identity: AActor::GetActorGuid() is WITH_EDITORONLY_DATA and the engine
// header documents it only as "available in editor builds" with no
// guarantee of stability across level save/reload for a freshly placed
// actor. Rather than assume a stability that isn't documented, the key is
// caller-controlled: `stable_key` if given, else `actor_name`. Both
// serialize/restore resolve the actor the same way `mcp_common::FindActorByName`
// already does everywhere else (object name OR editor label).
//
// Property filter: no NATIVE UPROPERTY of this plugin sets CPF_SaveGame, nor
// do the stock engine actor classes used in practice, so filtering by that
// flag alone would still save almost nothing for a generic actor. A BLUEPRINT
// variable's "SaveGame" checkbox does set it (the compiler copies it onto the
// generated FProperty), but whether a given actor's Blueprint ticked that box
// is not something this command can assume. Honest fallback stays an explicit
// `property_names` list; if omitted, only the transform is captured (still
// useful, and never silently empty).
// ============================================================================

namespace
{
    // Actor state is world-scoped, not per-player: every entry uses SaveGame UserIndex 0.
    constexpr int32 ActorStateUserIndex = 0;

    // Deterministic SaveGame slot name for an actor-state entry. Sanitizes
    // slashes out of the caller-supplied key: SaveGameToSlot's slot name
    // becomes part of a file path (ProjectSavedDir()/SaveGames/<Name>.sav),
    // and FGenericSaveGameSystem does not create intermediate directories.
    FString MakeActorStateSlotName(const FString& StableKey)
    {
        FString Sanitized = StableKey;
        Sanitized.ReplaceCharInline(TEXT('/'), TEXT('_'));
        Sanitized.ReplaceCharInline(TEXT('\\'), TEXT('_'));
        return TEXT("actor_state_") + Sanitized;
    }

    // Slot payload shape: {"actor_name","stable_key","transform":{loc,rot,scale},"properties":{name:jsonvalue}}
    TSharedPtr<FJsonObject> BuildTransformJson(const FTransform& T)
    {
        TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
        J->SetArrayField(TEXT("location"), mcp_common::VectorToJsonArray(T.GetLocation()));
        J->SetArrayField(TEXT("rotation"), mcp_common::RotatorToJsonArray(T.Rotator()));
        J->SetArrayField(TEXT("scale"), mcp_common::VectorToJsonArray(T.GetScale3D()));
        return J;
    }

    bool ParseTransformJson(const TSharedPtr<FJsonObject>& J, FTransform& OutTransform)
    {
        if (!J.IsValid()) { return false; }
        const TArray<TSharedPtr<FJsonValue>>* LocArr = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* ScaleArr = nullptr;
        if (!J->TryGetArrayField(TEXT("location"), LocArr) || !LocArr || LocArr->Num() != 3) { return false; }
        if (!J->TryGetArrayField(TEXT("rotation"), RotArr) || !RotArr || RotArr->Num() != 3) { return false; }
        if (!J->TryGetArrayField(TEXT("scale"), ScaleArr) || !ScaleArr || ScaleArr->Num() != 3) { return false; }
        const FVector Loc((*LocArr)[0]->AsNumber(), (*LocArr)[1]->AsNumber(), (*LocArr)[2]->AsNumber());
        const FRotator Rot((*RotArr)[0]->AsNumber(), (*RotArr)[1]->AsNumber(), (*RotArr)[2]->AsNumber());
        const FVector Scale((*ScaleArr)[0]->AsNumber(), (*ScaleArr)[1]->AsNumber(), (*ScaleArr)[2]->AsNumber());
        OutTransform = FTransform(Rot, Loc, Scale);
        return true;
    }
}

// MCP:serialize_actor_state - Save an actor's transform plus an EXPLICIT set of properties into a SaveGame slot, keyed by an identity YOU choose (stable_key, defaulting to actor_name). Nothing is captured implicitly: name the properties you want in property_names or you get the transform and only the transform. Idempotent -- re-serializing the same key overwrites that slot, it never duplicates. Pair with restore_actor_state, which takes the same key.
//   @param actor_name - Required. Object name or editor label of the actor to serialize (resolved via mcp_common::FindActorByName).
//   @param stable_key - Optional. Identity used as the persistence key; defaults to actor_name. The caller owns this key on purpose: AActor::GetActorGuid() is editor-only and not documented as stable across level reloads, so no GUID key is auto-generated for you.
//   @param property_names - Optional array of UPROPERTY names to capture in addition to the transform. If omitted, ONLY the transform is saved -- it never silently saves "everything" via CPF_SaveGame, which no native property here sets and which, on a Blueprint variable, depends on whether that Blueprint ticked the SaveGame checkbox (get_blueprint_variable_detailed_info reports it): a filter this command cannot assume for you.
//   @returns { slot_id: string, saved: bool, property_count: int }
TSharedPtr<FJsonObject> HandleSerializeActorState(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }
    FString StableKey;
    if (!Params->TryGetStringField(TEXT("stable_key"), StableKey) || StableKey.IsEmpty())
    {
        StableKey = ActorName;
    }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("No editor world available (headless without an open level?)"))) { return NoWorld; }
    AActor* Actor = mcp_common::FindActorByName(World, ActorName);
    if (!Actor)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actor_name"), ActorName);
    Payload->SetStringField(TEXT("stable_key"), StableKey);
    Payload->SetObjectField(TEXT("transform"), BuildTransformJson(Actor->GetActorTransform()));

    int32 PropertyCount = 0;
    TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
    const TArray<TSharedPtr<FJsonValue>>* PropertyNamesArr = nullptr;
    if (Params->TryGetArrayField(TEXT("property_names"), PropertyNamesArr) && PropertyNamesArr)
    {
        for (const TSharedPtr<FJsonValue>& Entry : *PropertyNamesArr)
        {
            const FString PropName = Entry.IsValid() ? Entry->AsString() : FString();
            if (PropName.IsEmpty()) { continue; }
            FProperty* Property = Actor->GetClass()->FindPropertyByName(*PropName);
            if (!Property)
            {
                return mcp_common::MakeErrorReply(FString::Printf(TEXT("Property not found on actor '%s': %s"), *ActorName, *PropName));
            }
            void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Actor);
            TSharedPtr<FJsonValue> JsonVal = FJsonObjectConverter::UPropertyToJsonValue(Property, ValuePtr, 0, 0);
            if (!JsonVal.IsValid())
            {
                return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to serialize property '%s' on actor '%s'"), *PropName, *ActorName));
            }
            PropsObj->SetField(PropName, JsonVal);
            ++PropertyCount;
        }
    }
    Payload->SetObjectField(TEXT("properties"), PropsObj);

    FString PayloadStr;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&PayloadStr);
    FJsonSerializer::Serialize(Payload.ToSharedRef(), Writer);

    const FString SlotName = MakeActorStateSlotName(StableKey);
    UMCPSaveGame* SaveGameInstance = NewObject<UMCPSaveGame>(GetTransientPackage(), UMCPSaveGame::StaticClass());
    SaveGameInstance->Payload = PayloadStr;
    const bool bSaved = UGameplayStatics::SaveGameToSlot(SaveGameInstance, SlotName, ActorStateUserIndex);

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("slot_id"), SlotName);
    Out->SetNumberField(TEXT("property_count"), PropertyCount);
    return mcp_common::MakeSavedReply(Out, bSaved, TEXT("serialize_actor_state"));
}

// MCP:restore_actor_state - Restore the transform + properties a previous serialize_actor_state wrote, addressed by the SAME key it was saved under. Idempotent: restoring twice in a row re-applies the same values both times. It restores only what was captured -- if serialize_actor_state was called without property_names, this puts back the transform and nothing else.
//   @param actor_name_or_stable_key - Required. The stable_key used at serialize time (or the actor_name, when no explicit stable_key was passed to serialize_actor_state). The actor to restore ONTO is then resolved from the actor_name stored inside the saved payload, looked up in the current world.
//   @returns { restored: bool, properties_restored: [string], transform_restored: bool }
TSharedPtr<FJsonObject> HandleRestoreActorState(const TSharedPtr<FJsonObject>& Params)
{
    FString Key;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name_or_stable_key"), Key)) { return Missing; }

    const FString SlotName = MakeActorStateSlotName(Key);
    UMCPSaveGame* Loaded = Cast<UMCPSaveGame>(UGameplayStatics::LoadGameFromSlot(SlotName, ActorStateUserIndex));
    if (!Loaded)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("No saved actor state for key: %s"), *Key));
    }
    const FString PayloadStr = Loaded->Payload;

    TSharedPtr<FJsonObject> Payload;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PayloadStr);
    if (!FJsonSerializer::Deserialize(Reader, Payload) || !Payload.IsValid())
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Corrupt actor-state payload for key: %s"), *Key));
    }

    FString ActorName;
    if (!Payload->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Malformed actor-state payload for key '%s': missing actor_name"), *Key));
    }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("No editor world available (headless without an open level?)"))) { return NoWorld; }
    AActor* Actor = mcp_common::FindActorByName(World, ActorName);
    if (!Actor)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor referenced by saved state not found in current world: %s"), *ActorName));
    }

    // Modify() before mutating the existing actor's transform/properties below: none of
    // SetActorTransform or the raw reflection writes in mcp_common::SetObjectProperty mark
    // the actor's (OFPA) package dirty on their own, so without this the restore looks
    // live-correct but is silently dropped by the next save_level.
    Actor->Modify();

    bool bTransformRestored = false;
    const TSharedPtr<FJsonObject>* TransformObj = nullptr;
    if (Payload->TryGetObjectField(TEXT("transform"), TransformObj) && TransformObj)
    {
        FTransform T;
        if (ParseTransformJson(*TransformObj, T))
        {
            Actor->SetActorTransform(T);
            bTransformRestored = true;
        }
    }

    TArray<TSharedPtr<FJsonValue>> RestoredNames;
    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    if (Payload->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj)
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PropsObj)->Values)
        {
            const FString PropName = Pair.Key;
            const TSharedPtr<FJsonValue> PropValue = Pair.Value;
            FString ErrorMessage;
            if (mcp_common::SetObjectProperty(Actor, PropName, PropValue, ErrorMessage))
            {
                RestoredNames.Add(MakeShared<FJsonValueString>(PropName));
            }
            else
            {
                return mcp_common::MakeErrorReply(
                    FString::Printf(TEXT("Failed to restore property '%s' on actor '%s': %s"), *PropName, *ActorName, *ErrorMessage));
            }
        }
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetBoolField(TEXT("restored"), true);
    Out->SetArrayField(TEXT("properties_restored"), RestoredNames);
    Out->SetBoolField(TEXT("transform_restored"), bTransformRestored);
    return mcp_common::MakeOkReply(Out);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("restore_actor_state"), &HandleRestoreActorState },
    { TEXT("serialize_actor_state"), &HandleSerializeActorState },
};
} // namespace UEAgentActorStateCommands
