// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Subsystems/EngineSubsystem.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "Subsystems/Subsystem.h"
#include "Subsystems/WorldSubsystem.h"

/** The lifetime family of a USubsystem class, shared by the commands that report it. */
namespace UEAgentSubsystemLifetime
{
    /**
     * Walks the parent chain of Cls and returns its lifetime category: "Editor", "Engine", "GameInstance", "World"
     * or "LocalPlayer". "Unknown" when Cls is not a USubsystem or derives from none of those bases.
     */
    inline FString DetectLifetimeCategory(const UClass* Cls)
    {
        if (!Cls || !Cls->IsChildOf(USubsystem::StaticClass()))
        {
            return TEXT("Unknown");
        }
        const TPair<UClass*, const TCHAR*> LifetimeMap[] = {
            { UEditorSubsystem::StaticClass(),       TEXT("Editor") },
            { UEngineSubsystem::StaticClass(),       TEXT("Engine") },
            { UGameInstanceSubsystem::StaticClass(), TEXT("GameInstance") },
            { UWorldSubsystem::StaticClass(),        TEXT("World") },
            { ULocalPlayerSubsystem::StaticClass(),  TEXT("LocalPlayer") },
        };
        for (const TPair<UClass*, const TCHAR*>& Pair : LifetimeMap)
        {
            if (Cls->IsChildOf(Pair.Key))
            {
                return Pair.Value;
            }
        }
        return TEXT("Unknown");
    }
}
