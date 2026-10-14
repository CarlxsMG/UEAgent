// Engine enum values spelled for replies the way the engine names them.
#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/EngineTypes.h"

namespace mcp_common
{
    /** "Standalone", "DedicatedServer", "ListenServer" or "Client"; "Unknown" for any other value. */
    inline const TCHAR* NetModeLabel(const ENetMode Mode)
    {
        switch (Mode)
        {
        case NM_Standalone:      return TEXT("Standalone");
        case NM_DedicatedServer: return TEXT("DedicatedServer");
        case NM_ListenServer:    return TEXT("ListenServer");
        case NM_Client:          return TEXT("Client");
        default:                 return TEXT("Unknown");
        }
    }

    /** "Never", "Awake", "DormantAll", "DormantPartial" or "Initial"; "Unknown" for any other value. */
    inline const TCHAR* NetDormancyLabel(const ENetDormancy Dormancy)
    {
        switch (Dormancy)
        {
        case DORM_Never:          return TEXT("Never");
        case DORM_Awake:          return TEXT("Awake");
        case DORM_DormantAll:     return TEXT("DormantAll");
        case DORM_DormantPartial: return TEXT("DormantPartial");
        case DORM_Initial:        return TEXT("Initial");
        default:                  return TEXT("Unknown");
        }
    }

    /** "Static", "Stationary" or "Movable"; "Unknown" for any other value. */
    inline const TCHAR* MobilityLabel(const EComponentMobility::Type Mobility)
    {
        switch (Mobility)
        {
        case EComponentMobility::Static:     return TEXT("Static");
        case EComponentMobility::Stationary: return TEXT("Stationary");
        case EComponentMobility::Movable:    return TEXT("Movable");
        default:                             return TEXT("Unknown");
        }
    }
}
