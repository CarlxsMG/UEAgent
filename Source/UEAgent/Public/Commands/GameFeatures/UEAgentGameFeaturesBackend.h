// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * The Game Features commands' engine side. It lives in the optional UEAgentGameFeatures module, which links
 * the engine's GameFeatures plugin: that module is loaded only when the project enables Game Features, so
 * UEAgent itself neither requires the plugin nor turns it on (enabling it asks the project for an Asset
 * Manager rule for GameFeatureData). Without it, the five commands answer that Game Features is off.
 */
class UEAGENT_API IUEAgentGameFeaturesBackend
{
public:
    virtual ~IUEAgentGameFeaturesBackend() = default;

    virtual TSharedPtr<FJsonObject> Activate(const TSharedPtr<FJsonObject>& Params) = 0;
    virtual TSharedPtr<FJsonObject> Deactivate(const TSharedPtr<FJsonObject>& Params) = 0;
    virtual TSharedPtr<FJsonObject> Unload(const TSharedPtr<FJsonObject>& Params) = 0;
    virtual TSharedPtr<FJsonObject> GetState(const TSharedPtr<FJsonObject>& Params) = 0;
    virtual TSharedPtr<FJsonObject> List(const TSharedPtr<FJsonObject>& Params) = 0;

    /** The backend, loading UEAgentGameFeatures on first use when the project enables Game Features; null otherwise. */
    static IUEAgentGameFeaturesBackend* Get();

    /** Called by UEAgentGameFeatures on startup (the backend) and shutdown (null). */
    static void Set(IUEAgentGameFeaturesBackend* Backend);
};
