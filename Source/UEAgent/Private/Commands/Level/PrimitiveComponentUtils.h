// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/CollisionProfile.h"    // UCollisionProfile channel display names
#include "Engine/EngineTypes.h"         // ECollisionEnabled / ECollisionChannel / ECollisionResponse
#include "GameFramework/Actor.h"

// Primitive-component helpers shared by the placed-actor commands and the Blueprint-template collision command:
// adding a component to a placed actor, finding an actor's primitive component, and applying a collision profile,
// mode and channel responses.
namespace PrimitiveComponentUtils
{
    /**
     * Registers Component, adds it to the actor's instance components and attaches it under the actor's root (keeping
     * its relative transform), or makes it the root when the actor has none.
     */
    inline void RegisterInstanceComponent(AActor* Actor, USceneComponent* Component)
    {
        Component->RegisterComponent();
        Actor->AddInstanceComponent(Component);
        if (USceneComponent* Root = Actor->GetRootComponent())
        {
            Component->AttachToComponent(Root, FAttachmentTransformRules::KeepRelativeTransform);
        }
        else
        {
            Actor->SetRootComponent(Component);
        }
    }

    /**
     * The actor's primitive component named ComponentName, or its root component when ComponentName is empty (null when
     * the root is no primitive component). False, with OutError, when no primitive component has that name.
     */
    inline bool FindPrimitive(AActor* Actor, const FString& ActorName, const FString& ComponentName,
        UPrimitiveComponent*& OutPrim, FString& OutError)
    {
        if (ComponentName.IsEmpty())
        {
            OutPrim = Cast<UPrimitiveComponent>(Actor->GetRootComponent());
            return true;
        }
        TArray<UPrimitiveComponent*> Prims;
        Actor->GetComponents<UPrimitiveComponent>(Prims);
        for (UPrimitiveComponent* P : Prims)
        {
            if (P && P->GetName() == ComponentName)
            {
                OutPrim = P;
                return true;
            }
        }
        OutError = FString::Printf(TEXT("Component '%s' not found or not a UPrimitiveComponent on actor '%s'"),
            *ComponentName, *ActorName);
        return false;
    }

    /** Ignore, Overlap or Block, in any case; false for any other value. */
    inline bool ParseCollisionResponse(const FString& Name, ECollisionResponse& Out)
    {
        if (Name.Equals(TEXT("Ignore"), ESearchCase::IgnoreCase))
        {
            Out = ECR_Ignore;
            return true;
        }
        if (Name.Equals(TEXT("Overlap"), ESearchCase::IgnoreCase))
        {
            Out = ECR_Overlap;
            return true;
        }
        if (Name.Equals(TEXT("Block"), ESearchCase::IgnoreCase))
        {
            Out = ECR_Block;
            return true;
        }
        return false;
    }

    /**
     * Container index of the collision channel whose display name is DisplayName, project custom channels included, or
     * INDEX_NONE. UCollisionProfile::ReturnContainerIndexFromChannelName is not exported, so its exported inverse is
     * scanned over the channel table.
     */
    inline int32 CollisionChannelIndex(const FName& DisplayName)
    {
        UCollisionProfile* CollisionProfile = UCollisionProfile::Get();
        for (int32 i = 0; i < (int32)ECollisionChannel::ECC_MAX; ++i)
        {
            if (CollisionProfile->ReturnChannelNameFromContainerIndex(i) == DisplayName)
            {
                return i;
            }
        }
        return INDEX_NONE;
    }

    /**
     * Applies to Prim, each only when given and in this order: a collision profile (SetCollisionProfileName expands it
     * into the body's mode and channel responses, so what follows overrides it), a collision-enabled mode by value name,
     * and a { channel: response } map whose channels are ECC_* tokens or display names and whose responses are Ignore,
     * Overlap or Block. The profile and the mode are recorded in Data, each applied response in OutApplied. False, with
     * OutError, at the first unknown mode, channel or response; the settings applied before it stay applied.
     */
    inline bool ApplyCollisionSettings(UPrimitiveComponent* Prim, const FString& Profile, const FString& CollisionEnabled,
        const TSharedPtr<FJsonObject>* Responses, const TSharedPtr<FJsonObject>& Data,
        TArray<TSharedPtr<FJsonValue>>& OutApplied, FString& OutError)
    {
        if (!Profile.IsEmpty())
        {
            Prim->SetCollisionProfileName(FName(*Profile));
            Data->SetStringField(TEXT("profile"), Profile);
        }

        if (!CollisionEnabled.IsEmpty())
        {
            const int64 EnabledVal = StaticEnum<ECollisionEnabled::Type>()->GetValueByNameString(CollisionEnabled);
            if (EnabledVal == INDEX_NONE)
            {
                OutError = FString::Printf(
                    TEXT("Unknown collision_enabled '%s' (expected NoCollision/QueryOnly/PhysicsOnly/QueryAndPhysics)"), *CollisionEnabled);
                return false;
            }
            Prim->SetCollisionEnabled((ECollisionEnabled::Type)EnabledVal);
            Data->SetStringField(TEXT("collision_enabled"), CollisionEnabled);
        }

        if (Responses == nullptr || !Responses->IsValid())
        {
            return true;
        }
        for (const auto& Pair : (*Responses)->Values)
        {
            // FJsonObject::Values keys are UE::FSharedString; deref (*Key) -> TCHAR*.
            const FString ChanName(*Pair.Key);
            int32 Idx = INDEX_NONE;
            if (ChanName.StartsWith(TEXT("ECC_")))
            {
                Idx = (int32)StaticEnum<ECollisionChannel>()->GetValueByNameString(ChanName);
            }
            else
            {
                Idx = CollisionChannelIndex(FName(*ChanName));
            }
            if (Idx < 0)
            {
                // An unknown channel is refused rather than written as ECC_WorldStatic (index 0).
                OutError = FString::Printf(
                    TEXT("Unknown collision channel: %s (use a friendly channel display name or an ECC_* token)"), *ChanName);
                return false;
            }
            FString RespStr;
            if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(RespStr))
            {
                OutError = FString::Printf(TEXT("Response for channel '%s' must be a string"), *ChanName);
                return false;
            }
            ECollisionResponse Response = ECR_Ignore;
            if (!ParseCollisionResponse(RespStr, Response))
            {
                OutError = FString::Printf(
                    TEXT("Unknown collision response '%s' for channel '%s' (expected Ignore/Overlap/Block)"), *RespStr, *ChanName);
                return false;
            }
            Prim->SetCollisionResponseToChannel((ECollisionChannel)Idx, Response);
            TSharedPtr<FJsonObject> One = MakeShared<FJsonObject>();
            One->SetStringField(TEXT("channel"), ChanName);
            One->SetStringField(TEXT("response"), RespStr);
            OutApplied.Add(MakeShared<FJsonValueObject>(One));
        }
        return true;
    }
}
