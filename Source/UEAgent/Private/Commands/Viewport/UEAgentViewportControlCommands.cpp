#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Misc/EngineVersionComparison.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EditorViewportClient.h"
#include "Settings/EditorViewportSettings.h"
#include "LevelEditorViewport.h"
#include "ShowFlags.h"
#include "Engine/EngineBaseTypes.h"
#include "RenderUtils.h"                       // IsRayTracingAllowed, the editor's gate for Path Tracing
#include "Editor/EditorPerformanceSettings.h"   // UEditorPerformanceSettings::bThrottleCPUWhenNotForeground

/**
 * Editor viewport control commands.
 *
 * Operates on the active editor level viewport client (GEditor->GetLevelViewportClients() /
 * GCurrentLevelEditingViewportClient). Provides realtime toggle, view-mode selection, engine show-flag
 * toggling, camera speed, camera transform and a full viewport-state readback, plus FOV, clip planes,
 * exposure, game view, cinematic control and view type.
 */
namespace UEAgentViewportControlCommands
{

// MCP-DOMAIN: Editor

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{
    /**
     * Resolve the active editor level viewport client. Prefers the current
     * level-editing viewport client (the one with focus), then falls back to
     * the first registered level viewport client. Returns nullptr if none
     * exist (e.g. running with -nullrhi and no viewport).
     */
    FLevelEditorViewportClient* ResolveViewportClient()
    {
        if (GCurrentLevelEditingViewportClient)
        {
            return GCurrentLevelEditingViewportClient;
        }
        if (GEditor)
        {
            const TArray<FLevelEditorViewportClient*>& Clients = GEditor->GetLevelViewportClients();
            for (FLevelEditorViewportClient* Client : Clients)
            {
                if (Client)
                {
                    return Client;
                }
            }
        }
        return nullptr;
    }

    /** ResolveViewportClient's client; null on success, else the error reply. */
    TSharedPtr<FJsonObject> ViewportControl_RequireClient(FEditorViewportClient*& OutClient)
    {
        OutClient = ResolveViewportClient();
        if (!OutClient)
        {
            return mcp_common::MakeErrorReply(TEXT("No active editor viewport available"));
        }
        return nullptr;
    }

    /**
     * True when Field holds exactly three JSON numbers. mcp_common::ReadJsonVector reads any
     * other shape (a short array, strings, an object) as (0, 0, 0), which would put the camera
     * at the world origin instead of reporting the mistake.
     */
    bool ViewportControl_IsNumberTriple(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field)
    {
        const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
        if (!Params->TryGetArrayField(Field, Items) || !Items || Items->Num() != 3)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Item : *Items)
        {
            if (!Item.IsValid() || Item->Type != EJson::Number)
            {
                return false;
            }
        }
        return true;
    }

    /**
     * True when Field holds a rotation mcp_common::ReadJsonRotator reads as given: three numbers
     * [pitch, yaw, roll], or an object with at least one numeric roll, pitch or yaw. Anything
     * else reads as a zero rotation.
     */
    bool ViewportControl_IsRotation(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field)
    {
        if (ViewportControl_IsNumberTriple(Params, Field))
        {
            return true;
        }
        const TSharedPtr<FJsonObject>* Axes = nullptr;
        if (Params->TryGetObjectField(Field, Axes) && Axes && Axes->IsValid())
        {
            double Unused = 0.0;
            return (*Axes)->TryGetNumberField(TEXT("roll"), Unused)
                || (*Axes)->TryGetNumberField(TEXT("pitch"), Unused)
                || (*Axes)->TryGetNumberField(TEXT("yaw"), Unused);
        }
        return false;
    }

    /**
     * An engine view mode: the name the readers report, another spelling the setter also accepts (or null), and
     * why set_viewport_view_mode refuses it (null when it sets it).
     */
    struct FViewportControl_ViewModeName
    {
        EViewModeIndex Index;
        const TCHAR* Name;
        const TCHAR* Alias;
        const TCHAR* SetupNeeded;
    };

    /**
     * Every view mode of UE 5.8's EViewModeIndex but the deprecated Lit Wireframe, each with ONE name: its engine
     * display name (the UENUM DisplayName) without spaces or punctuation, the one the readers report and the setter takes
     * back, so a name read from get_viewport_state always selects the same mode again. Wireframe is
     * VMI_BrushWireframe and CSGWireframe is VMI_Wireframe, as those display names put it.
     *
     * The setter sets a mode the way the editor's own view mode command does, with SetViewMode alone. A mode the
     * editor only enters together with a choice of what to show -- a buffer, a visualization, texture streaming
     * data built for the level -- carries the reason it is refused: SetViewMode alone would show nothing useful,
     * or keep the choice the viewport made last.
     */
    const FViewportControl_ViewModeName ViewportControl_ViewModes[] =
    {
        { VMI_Lit,                              TEXT("Lit"),                           nullptr,                       nullptr },
        { VMI_Unlit,                            TEXT("Unlit"),                         nullptr,                       nullptr },
        { VMI_BrushWireframe,                   TEXT("Wireframe"),                     TEXT("BrushWireframe"),        nullptr },
        { VMI_Wireframe,                        TEXT("CSGWireframe"),                  nullptr,                       nullptr },
        { VMI_Lit_DetailLighting,               TEXT("DetailLighting"),                nullptr,                       nullptr },
        { VMI_LightingOnly,                     TEXT("LightingOnly"),                  nullptr,                       nullptr },
        { VMI_LightComplexity,                  TEXT("LightComplexity"),               nullptr,                       nullptr },
        { VMI_ShaderComplexity,                 TEXT("ShaderComplexity"),              nullptr,                       nullptr },
        { VMI_LightmapDensity,                  TEXT("LightmapDensity"),               nullptr,                       nullptr },
        { VMI_LitLightmapDensity,               TEXT("LitLightmapDensity"),            nullptr,                       nullptr },
        { VMI_ReflectionOverride,               TEXT("Reflections"),                   TEXT("ReflectionOverride"),    nullptr },
        { VMI_StationaryLightOverlap,           TEXT("StationaryLightOverlap"),        nullptr,                       nullptr },
        { VMI_CollisionPawn,                    TEXT("PlayerCollision"),               nullptr,                       nullptr },
        { VMI_CollisionVisibility,              TEXT("VisibilityCollision"),           nullptr,                       nullptr },
        { VMI_LODColoration,                    TEXT("MeshLODColoration"),             nullptr,                       nullptr },
        { VMI_HLODColoration,                   TEXT("HierarchicalLODColoration"),     nullptr,                       nullptr },
        { VMI_QuadOverdraw,                     TEXT("QuadOverdraw"),                  nullptr,                       nullptr },
        { VMI_ShaderComplexityWithQuadOverdraw, TEXT("ShaderComplexityQuads"),         nullptr,                       nullptr },
        { VMI_LWCComplexity,                    TEXT("MaterialLWCFunctionUsage"),      nullptr,                       nullptr },
        { VMI_PathTracing,                      TEXT("PathTracing"),                   nullptr,                       nullptr },
        { VMI_Clay,                             TEXT("Clay"),                          nullptr,                       nullptr },
        { VMI_FrontBackFace,                    TEXT("FrontBackFace"),                 nullptr,                       nullptr },
        { VMI_RandomColor,                      TEXT("Random"),                        TEXT("RandomColor"),           nullptr },
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
        // The four streaming modes and MegaLights Visualization are values 5.7's EViewModeIndex does not have.
        { VMI_StreamingTextureDeficit,          TEXT("StreamingTextureDeficit"),       nullptr,                       nullptr },
        { VMI_StreamingTextureResidency,        TEXT("StreamingTextureResidency"),     nullptr,                       nullptr },
        { VMI_StreamingMeshLODDeficit,          TEXT("StreamingMeshLODDeficit"),       nullptr,                       nullptr },
        { VMI_StreamingMeshLODResidency,        TEXT("StreamingMeshLODResidency"),     nullptr,                       nullptr },
#endif
        { VMI_VisualizeBuffer,                  TEXT("BufferVisualization"),           nullptr,
            TEXT("it shows one buffer, picked in the editor's Buffer Visualization menu") },
        { VMI_VisualizeNanite,                  TEXT("NaniteVisualization"),           nullptr,
            TEXT("it shows one Nanite visualization, picked in the editor's Nanite Visualization menu") },
        { VMI_VisualizeLumen,                   TEXT("LumenVisualization"),            nullptr,
            TEXT("it shows one Lumen visualization, picked in the editor's Lumen menu") },
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
        { VMI_VisualizeMegaLights,              TEXT("MegaLightsVisualization"),       nullptr,
            TEXT("it shows one MegaLights visualization, picked in the editor's MegaLights menu") },
#endif
        { VMI_VisualizeSubstrate,               TEXT("SubstrateVisualization"),        nullptr,
            TEXT("it shows one Substrate visualization, picked in the editor's Substrate menu") },
        { VMI_VisualizeGroom,                   TEXT("GroomVisualization"),            nullptr,
            TEXT("it shows one groom visualization, picked in the editor's Groom menu") },
        { VMI_VisualizeVirtualShadowMap,        TEXT("VirtualShadowMapVisualization"), nullptr,
            TEXT("it shows one virtual shadow map visualization, picked in the editor's Virtual Shadow Map menu") },
        { VMI_ShadowCasters,                    TEXT("ShadowCastersVisualization"),    nullptr,
            TEXT("the editor enters it as the Shadow Casters entry of its Virtual Shadow Map menu, which also picks that visualization") },
        { VMI_VisualizeVirtualTexture,          TEXT("VirtualTextureVisualization"),   nullptr,
            TEXT("it shows one virtual texture visualization, picked in the editor's Virtual Texture menu") },
        { VMI_VisualizeGPUSkinCache,            TEXT("GPUSkinCacheVisualization"),     nullptr,
            TEXT("it shows one GPU skin cache visualization, picked in the editor's GPU Skin Cache menu") },
        { VMI_VisualizeActorColoration,         TEXT("ActorColorationVisualization"),  nullptr,
            TEXT("it colors actors by one handler, picked in the editor's Actor Coloration menu") },
        { VMI_RayTracingDebug,                  TEXT("RayTracingDebug"),               nullptr,
            TEXT("it shows one ray tracing debug view, picked in the editor's Ray Tracing Debug menu") },
        { VMI_Zebra,                            TEXT("Zebra"),                         nullptr,
            TEXT("its stripes run horizontally or vertically, picked by the editor's two Zebra entries") },
        { VMI_GroupLODColoration,               TEXT("GroupLODColoration"),            nullptr,
            TEXT("it is the editor's menu entry grouping MeshLODColoration and HierarchicalLODColoration, not a mode of its own") },
        { VMI_PrimitiveDistanceAccuracy,        TEXT("PrimitiveDistance"),             nullptr,
            TEXT("this texture streaming accuracy mode shows the data Build Texture Streaming computes for the level") },
        { VMI_MeshUVDensityAccuracy,            TEXT("MeshUVDensity"),                 nullptr,
            TEXT("this texture streaming accuracy mode shows the data Build Texture Streaming computes for the level, per UV channel") },
        { VMI_MaterialTextureScaleAccuracy,     TEXT("MaterialTextureScales"),         nullptr,
            TEXT("this texture streaming accuracy mode rebuilds the material texture streaming bindings of the level when it is entered") },
        { VMI_RequiredTextureResolution,        TEXT("RequiredTextureResolution"),     nullptr,
            TEXT("it compares one texture, picked from the materials selected in the Content Browser") },
    };

    /** The table entry of Index, or null for a value the table does not name. */
    const FViewportControl_ViewModeName* ViewportControl_FindMode(EViewModeIndex Index)
    {
        for (const FViewportControl_ViewModeName& Mode : ViewportControl_ViewModes)
        {
            if (Mode.Index == Index)
            {
                return &Mode;
            }
        }
        return nullptr;
    }

    /** The table entry whose name or alias is Name (without case), or null. */
    const FViewportControl_ViewModeName* ViewportControl_FindModeByName(const FString& Name)
    {
        for (const FViewportControl_ViewModeName& Mode : ViewportControl_ViewModes)
        {
            if (Name.Equals(Mode.Name, ESearchCase::IgnoreCase)
                || (Mode.Alias && Name.Equals(Mode.Alias, ESearchCase::IgnoreCase)))
            {
                return &Mode;
            }
        }
        return nullptr;
    }

    /** The engine display name of an EViewModeIndex value without spaces or punctuation; empty when it has none. */
    FString ViewportControl_ReflectedModeName(int64 Value)
    {
        const UEnum* Enum = StaticEnum<EViewModeIndex>();
        const int32 EnumIndex = Enum ? Enum->GetIndexByValue(Value) : INDEX_NONE;
        if (EnumIndex == INDEX_NONE)
        {
            return FString();
        }
        FString Name;
        for (const TCHAR Char : Enum->GetMetaData(TEXT("DisplayName"), EnumIndex))
        {
            if (FChar::IsAlnum(Char))
            {
                Name.AppendChar(Char);
            }
        }
        return Name;
    }

    /**
     * The name of a view mode as the readers report it: the table's name, else the engine display name of a value
     * the table does not cover (the deprecated Lit Wireframe, a value of a later engine), else ViewMode_<index>.
     */
    FString ViewModeIndexToName(EViewModeIndex Index)
    {
        if (const FViewportControl_ViewModeName* Mode = ViewportControl_FindMode(Index))
        {
            return Mode->Name;
        }
        const FString Reflected = ViewportControl_ReflectedModeName(static_cast<int64>(Index));
        return Reflected.IsEmpty() ? FString::Printf(TEXT("ViewMode_%d"), (int32)Index) : Reflected;
    }

    /** Every name set_viewport_view_mode sets, with the aliases, comma-separated, for its errors. */
    FString ViewportControl_ValidViewModeNames()
    {
        TArray<FString> Names;
        for (const FViewportControl_ViewModeName& Mode : ViewportControl_ViewModes)
        {
            if (Mode.SetupNeeded)
            {
                continue;
            }
            Names.Add(Mode.Name);
            if (Mode.Alias)
            {
                Names.Add(Mode.Alias);
            }
        }
        return FString::Join(Names, TEXT(", "));
    }

    /**
     * The view mode set_viewport_view_mode sets for Name, or the error reply when it sets none: an unknown name, a
     * mode that needs a choice the editor makes along with it, Path Tracing where ray tracing is not allowed, or a
     * mode the engine hides. Checked before any viewport is touched.
     */
    TSharedPtr<FJsonObject> ViewportControl_ResolveSettableMode(const FString& Name, EViewModeIndex& OutIndex)
    {
        if (const FViewportControl_ViewModeName* Mode = ViewportControl_FindModeByName(Name))
        {
            if (Mode->SetupNeeded)
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("set_viewport_view_mode does not set '%s': %s. Nothing was changed. Modes it sets: %s"),
                    Mode->Name, Mode->SetupNeeded, *ViewportControl_ValidViewModeNames()));
            }
            // The editor binds its Path Tracing command only where ray tracing is allowed.
            if (Mode->Index == VMI_PathTracing && !IsRayTracingAllowed())
            {
                return mcp_common::MakeErrorReply(TEXT("view_mode 'PathTracing' needs ray tracing, which this editor does not allow ")
                    TEXT("(r.RayTracing off or an RHI without it); the editor offers Path Tracing only then. Nothing was changed."));
            }
            OutIndex = Mode->Index;
            return nullptr;
        }
        // A name the readers can report outside the table: refused, saying why.
        const UEnum* Enum = StaticEnum<EViewModeIndex>();
        for (int32 EnumIndex = 0; Enum && EnumIndex < Enum->NumEnums(); ++EnumIndex)
        {
            const FString Reflected = ViewportControl_ReflectedModeName(Enum->GetValueByIndex(EnumIndex));
            if (Reflected.IsEmpty() || !Name.Equals(Reflected, ESearchCase::IgnoreCase))
            {
                continue;
            }
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("set_viewport_view_mode does not set '%s': it is %s. Nothing was changed. Modes it sets: %s"), *Reflected,
                Enum->HasMetaData(TEXT("Hidden"), EnumIndex)
                    ? TEXT("hidden by the engine (deprecated, or not a mode of its own)")
                    : TEXT("an engine view mode this tool does not set"),
                *ViewportControl_ValidViewModeNames()));
        }
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown view_mode '%s'. Valid: %s"), *Name, *ViewportControl_ValidViewModeNames()));
    }

    /** The range of the level viewport's Field of View control (the editor clamps its own FOV setters to it). */
    constexpr double ViewportControl_MinFOV = 5.0;
    constexpr double ViewportControl_MaxFOV = 170.0;

    /**
     * The smallest near plane the editor's Near View Plane control allows. A near plane of 0 makes the
     * reversed-Z perspective projection degenerate (every depth comes out 0), so 0 up to this is refused.
     */
    constexpr double ViewportControl_MinNearClipPlane = 0.001;

    /** The name this API uses for a level viewport type; "Unknown" for any other value. */
    FString ViewportControl_ViewTypeName(ELevelViewportType Type)
    {
        switch (Type)
        {
        case LVT_Perspective:    return TEXT("Perspective");
        case LVT_OrthoTop:       return TEXT("Top");
        case LVT_OrthoBottom:    return TEXT("Bottom");
        case LVT_OrthoLeft:      return TEXT("Left");
        case LVT_OrthoRight:     return TEXT("Right");
        case LVT_OrthoFront:     return TEXT("Front");
        case LVT_OrthoBack:      return TEXT("Back");
        case LVT_OrthoFreelook:  return TEXT("Freelook");
        default:                 return TEXT("Unknown");
        }
    }
}

// ---------------------------------------------------------------------------
// Lifecycle / routing

// ---------------------------------------------------------------------------
// set_viewport_realtime
// ---------------------------------------------------------------------------

// MCP:set_viewport_realtime - Enable or disable realtime rendering on the active editor viewport
//   @param enabled - True to turn on realtime updates, false to turn them off
//   @returns { applied: bool, requested: bool, realtime: bool, effective_realtime: bool, override_active: bool, override_source: string }
TSharedPtr<FJsonObject> HandleSetViewportRealtime(const TSharedPtr<FJsonObject>& Params)
{
    bool bEnabled = true;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredBool(Params, TEXT("enabled"), bEnabled)) { return Missing; }

    FEditorViewportClient* Client = nullptr;
    if (TSharedPtr<FJsonObject> Error = ViewportControl_RequireClient(Client)) { return Error; }

    // Why "set true" can read back realtime:false headless: on every
    // tick UEditorEngine::Tick re-injects a `false` realtime OVERRIDE named
    // "Background Process" on each viewport client while the editor lacks OS focus
    // AND UEditorPerformanceSettings::bThrottleCPUWhenNotForeground is true (its
    // default). FEditorViewportClient::IsRealtime() returns the override when one
    // is set, masking SetRealtime(true). When enabling realtime we disable that
    // throttle (exactly what Epic's automation framework does) so realtime is
    // actually effective headless, and clear the override already present this
    // tick — RemoveRealtimeOverride alone wouldn't stick (the next tick re-adds it
    // unless the throttle setting is off).
    if (bEnabled)
    {
        GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground = false;
        Client->RemoveRealtimeOverride(FText::FromString(TEXT("Background Process")),
                                       /*bCheckMissingOverride=*/false);
    }

    Client->SetRealtime(bEnabled);
    Client->Invalidate();

    // Honest read-back. `realtime` is the TRUE base flag (SaveRealtimeStateToConfig
    // ignores overrides); `effective_realtime` is the net value IsRealtime() returns
    // (base flag folded with any active override); `override_active`/`override_source`
    // expose an override still masking the base flag so the result is never a lie.
    bool bRawRealtime = false;
    Client->SaveRealtimeStateToConfig(bRawRealtime);
    const bool bOverrideActive = Client->IsRealtimeOverrideSet();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetBoolField(TEXT("requested"), bEnabled);
    Data->SetBoolField(TEXT("realtime"), bRawRealtime);
    Data->SetBoolField(TEXT("effective_realtime"), Client->IsRealtime());
    Data->SetBoolField(TEXT("override_active"), bOverrideActive);
    if (bOverrideActive)
    {
        Data->SetStringField(TEXT("override_source"), Client->GetRealtimeOverrideMessage().ToString());
    }
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_viewport_view_mode
// ---------------------------------------------------------------------------

// MCP:set_viewport_view_mode - Set the rendering view mode (Lit, Unlit, Wireframe, ShaderComplexity...) of the active level editor viewport, the focused one or else the first; no viewport is an error. view_mode is matched without case. An unknown name is an error before anything changes, and so is a mode the editor only enters with a choice of what to show (a buffer, a visualization, texture streaming data, a zebra direction): the error says which. The mode is stored for the viewport's current projection only (perspective and orthographic keep separate modes) and lasts for this editor session: no undo step, nothing saved. view_mode and view_mode_index are read back, view_mode as the name get_viewport_state reports and this tool takes back. Show flags: set_show_flag; projection: set_view_type; game view: set_game_view.
//   @param view_mode - Engine view mode name, matched without case, e.g. Lit, Unlit, Wireframe, CSGWireframe, DetailLighting, LightingOnly, LightComplexity, ShaderComplexity, LightmapDensity, Reflections, PlayerCollision, MeshLODColoration, QuadOverdraw, Clay; each is the engine display name without spaces. BrushWireframe is accepted for Wireframe, ReflectionOverride for Reflections, RandomColor for Random; PathTracing only where ray tracing is allowed.
//   @returns { applied: bool, requested: string, view_mode_index: int, view_mode: string }
TSharedPtr<FJsonObject> HandleSetViewportViewMode(const TSharedPtr<FJsonObject>& Params)
{
    FString ViewModeStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("view_mode"), ViewModeStr, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    EViewModeIndex DesiredIndex = VMI_Lit;
    if (TSharedPtr<FJsonObject> Refused = ViewportControl_ResolveSettableMode(ViewModeStr, DesiredIndex)) { return Refused; }

    FEditorViewportClient* Client = nullptr;
    if (TSharedPtr<FJsonObject> Error = ViewportControl_RequireClient(Client)) { return Error; }

    Client->SetViewMode(DesiredIndex);
    Client->Invalidate();

    const EViewModeIndex ReadBack = Client->GetViewMode();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetStringField(TEXT("requested"), ViewModeStr);
    Data->SetNumberField(TEXT("view_mode_index"), (int32)ReadBack);
    Data->SetStringField(TEXT("view_mode"), ViewModeIndexToName(ReadBack));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_show_flag
// ---------------------------------------------------------------------------

// MCP:set_show_flag - Set one engine show flag (Grid, Bounds, Collision, Wireframe, Lighting...) on the active level editor viewport, the focused one or else the first; no viewport is an error. It sets the given state rather than toggling: enabled true turns the flag on, false turns it off, and a repeat call changes nothing. flag_name is matched without case against the engine's show flags and registered custom flags; an unknown name is an error before anything changes. It lasts for this editor session, with no undo step, nothing saved and the earlier value not returned. value is the flag read back and flag_index its engine index. get_viewport_state reads Grid, Bounds, Collision, Wireframe and Lighting back; set_game_view swaps the whole flag set; set_viewport_view_mode changes the render mode.
//   @param flag_name - Engine show flag name as FEngineShowFlags spells it, e.g. Grid, Bounds, Collision, Wireframe, Lighting; matched without case
//   @param enabled - True to set the flag, false to clear it
//   @returns { applied: bool, flag_name: string, flag_index: int, requested: bool, value: bool }
TSharedPtr<FJsonObject> HandleSetShowFlag(const TSharedPtr<FJsonObject>& Params)
{
    FString FlagName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("flag_name"), FlagName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    bool bEnabled = true;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredBool(Params, TEXT("enabled"), bEnabled)) { return Missing; }

    const int32 FlagIndex = FEngineShowFlags::FindIndexByName(*FlagName);
    if (FlagIndex == INDEX_NONE)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown show flag '%s'"), *FlagName));
    }

    FEditorViewportClient* Client = nullptr;
    if (TSharedPtr<FJsonObject> Error = ViewportControl_RequireClient(Client)) { return Error; }

    Client->EngineShowFlags.SetSingleFlag((uint32)FlagIndex, bEnabled);
    Client->Invalidate();

    const bool bReadBack = Client->EngineShowFlags.GetSingleFlag((uint32)FlagIndex);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetStringField(TEXT("flag_name"), FlagName);
    Data->SetNumberField(TEXT("flag_index"), FlagIndex);
    Data->SetBoolField(TEXT("requested"), bEnabled);
    Data->SetBoolField(TEXT("value"), bReadBack);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_camera_speed
// ---------------------------------------------------------------------------

// MCP:set_camera_speed - Set the camera movement speed of the active level editor viewport (the focused one, else the first). Affects this editor session only and saves nothing. Returns requested and camera_speed, the value read back from the viewport afterwards.
//   @param speed - Camera speed as a positive float; 0 or less is an error. Compare camera_speed in the reply to see what the viewport kept.
//   @returns { applied: bool, requested: number, camera_speed: number }
TSharedPtr<FJsonObject> HandleSetCameraSpeed(const TSharedPtr<FJsonObject>& Params)
{
    double SpeedNum = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("speed"), SpeedNum)) { return Missing; }

    if (SpeedNum <= 0.0)
    {
        return mcp_common::MakeErrorReply(TEXT("speed must be a positive number"));
    }

    FEditorViewportClient* Client = nullptr;
    if (TSharedPtr<FJsonObject> Error = ViewportControl_RequireClient(Client)) { return Error; }

    // Integer 1-8 camera-speed settings are deprecated (5.7); the camera speed is a float now.
    Client->SetCameraSpeedSettings(FEditorViewportCameraSpeedSettings((float)SpeedNum));
    const float ReadBack = Client->GetCameraSpeed();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetNumberField(TEXT("requested"), SpeedNum);
    Data->SetNumberField(TEXT("camera_speed"), ReadBack);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_viewport_camera
// ---------------------------------------------------------------------------

// MCP:set_viewport_camera - Place the camera of the active level editor viewport (the focused one, else the first) at an absolute world location and/or rotation; at least one is required and no viewport is an error. Editor view only, not undoable or saved: a PIE player camera (set_view_target) or a camera actor (set_camera_transform) does not move. location must be an [x, y, z] array of three numbers in cm and rotation three numbers or a roll/pitch/yaw object; anything else is refused before the camera moves. The reply reads location and rotation back, rotation as [pitch, yaw, roll]. Frame an actor with focus_viewport.
//   @param location - Optional [x, y, z] world location
//   @param rotation - Optional [pitch, yaw, roll] world rotation
//   @returns { applied: bool, location_set: bool, rotation_set: bool, location: [number], rotation: [number] }
TSharedPtr<FJsonObject> HandleSetViewportCamera(const TSharedPtr<FJsonObject>& Params)
{
    FEditorViewportClient* Client = nullptr;
    if (TSharedPtr<FJsonObject> Error = ViewportControl_RequireClient(Client)) { return Error; }

    bool bAnythingSet = false;
    bool bLocationSet = false;
    bool bRotationSet = false;

    // Both shapes are checked before the camera moves, so a bad rotation does not leave a
    // location already applied.
    if (Params->HasField(TEXT("location")) && !ViewportControl_IsNumberTriple(Params, TEXT("location")))
    {
        return mcp_common::MakeErrorReply(
            TEXT("location must be an array of three numbers [x, y, z] in cm; the camera was not moved"));
    }
    if (Params->HasField(TEXT("rotation")) && !ViewportControl_IsRotation(Params, TEXT("rotation")))
    {
        return mcp_common::MakeErrorReply(
            TEXT("rotation must be an array of three numbers [pitch, yaw, roll] or an object with roll, pitch and/or yaw; the camera was not moved"));
    }

    if (Params->HasField(TEXT("location")))
    {
        const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));
        Client->SetViewLocation(Location);
        bLocationSet = true;
        bAnythingSet = true;
    }

    if (Params->HasField(TEXT("rotation")))
    {
        const FRotator Rotation = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));
        Client->SetViewRotation(Rotation);
        bRotationSet = true;
        bAnythingSet = true;
    }

    if (!bAnythingSet)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Provide at least one of: location, rotation"));
    }

    Client->Invalidate();

    const FVector ReadLoc = Client->GetViewLocation();
    const FRotator ReadRot = Client->GetViewRotation();

    TArray<TSharedPtr<FJsonValue>> LocArr = mcp_common::VectorToJsonArray(ReadLoc);

    TArray<TSharedPtr<FJsonValue>> RotArr = mcp_common::RotatorToJsonArray(ReadRot);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetBoolField(TEXT("location_set"), bLocationSet);
    Data->SetBoolField(TEXT("rotation_set"), bRotationSet);
    Data->SetArrayField(TEXT("location"), LocArr);
    Data->SetArrayField(TEXT("rotation"), RotArr);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_viewport_state
// ---------------------------------------------------------------------------

// MCP:get_viewport_state - Read the active level editor viewport (the focused one, else the first) without changing it: realtime (the effective value, an override such as the background throttle included), view mode name and index, camera speed, camera location [x, y, z] and rotation [pitch, yaw, roll], and the Grid, Bounds, Collision, Wireframe and Lighting show flags, a flag this engine lacks left out. view_mode names every engine view mode by its display name without spaces (Lit, CSGWireframe, PlayerCollision...), or ViewMode_<index> for a value the engine does not name; set_viewport_view_mode takes that name back, but refuses the modes that need a choice made with them. It also reads the perspective FOV, the near clip plane and far clip override, the exposure, the view type (perspective or an orthographic view) and whether cinematic tools may possess the viewport. No viewport is an error; nothing is saved or recorded for undo. Setters: set_viewport_view_mode, set_viewport_realtime, set_camera_speed, set_viewport_camera, set_show_flag, set_viewport_fov, set_near_clip_plane, set_far_clip_plane, set_viewport_exposure, set_view_type, set_allow_cinematic_control.
//   @returns { realtime: bool, view_mode_index: int, view_mode: string, camera_speed: number, location: [number], rotation: [number], show_flags: object, fov: number (current perspective FOV, degrees), near_clip_plane: number (effective, cm), far_clip_plane_override: number (< 0 when no override is engaged), exposure: { fixed: bool, ev100: number }, view_type: string, view_type_index: int, allows_cinematic_control: bool }
TSharedPtr<FJsonObject> HandleGetViewportState(const TSharedPtr<FJsonObject>& Params)
{
    FEditorViewportClient* Client = nullptr;
    if (TSharedPtr<FJsonObject> Error = ViewportControl_RequireClient(Client)) { return Error; }

    const FVector Loc = Client->GetViewLocation();
    const FRotator Rot = Client->GetViewRotation();
    const EViewModeIndex VMode = Client->GetViewMode();

    TArray<TSharedPtr<FJsonValue>> LocArr = mcp_common::VectorToJsonArray(Loc);

    TArray<TSharedPtr<FJsonValue>> RotArr = mcp_common::RotatorToJsonArray(Rot);

    // Read back a handful of common show flags by name.
    TSharedPtr<FJsonObject> ShowFlagsObj = MakeShared<FJsonObject>();
    const TCHAR* CommonFlags[] = { TEXT("Grid"), TEXT("Bounds"), TEXT("Collision"),
                                   TEXT("Wireframe"), TEXT("Lighting") };
    for (const TCHAR* FlagName : CommonFlags)
    {
        const int32 Idx = FEngineShowFlags::FindIndexByName(FlagName);
        if (Idx != INDEX_NONE)
        {
            ShowFlagsObj->SetBoolField(FlagName, Client->EngineShowFlags.GetSingleFlag((uint32)Idx));
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("realtime"), Client->IsRealtime());
    Data->SetNumberField(TEXT("view_mode_index"), (int32)VMode);
    Data->SetStringField(TEXT("view_mode"), ViewModeIndexToName(VMode));
    Data->SetNumberField(TEXT("camera_speed"), Client->GetCameraSpeed());
    Data->SetArrayField(TEXT("location"), LocArr);
    Data->SetArrayField(TEXT("rotation"), RotArr);
    Data->SetObjectField(TEXT("show_flags"), ShowFlagsObj);
    Data->SetNumberField(TEXT("fov"), Client->ViewFOV);
    Data->SetNumberField(TEXT("near_clip_plane"), Client->GetNearClipPlane());
    // Far override is < 0 when no override is engaged (falls back to GFarClippingPlane).
    Data->SetNumberField(TEXT("far_clip_plane_override"), Client->GetFarClipPlaneOverride());
    TSharedPtr<FJsonObject> ExposureObj = MakeShared<FJsonObject>();
    ExposureObj->SetBoolField(TEXT("fixed"), Client->ExposureSettings.bFixed);
    ExposureObj->SetNumberField(TEXT("ev100"), Client->ExposureSettings.FixedEV100);
    Data->SetObjectField(TEXT("exposure"), ExposureObj);
    const ELevelViewportType ViewType = Client->GetViewportType();
    Data->SetStringField(TEXT("view_type"), ViewportControl_ViewTypeName(ViewType));
    Data->SetNumberField(TEXT("view_type_index"), (int32)ViewType);
    Data->SetBoolField(TEXT("allows_cinematic_control"), Client->AllowsCinematicControl());
    return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// FOV, clip planes, exposure, game view, cinematic control, view type.
// All headless-friendly except set_viewport_layout (Slate-dependent — omitted).

// ---------------------------------------------------------------------------
// set_viewport_fov
// ---------------------------------------------------------------------------

// MCP:set_viewport_fov - Set the perspective field of view, in degrees, of the active level editor viewport, the focused one or else the first; no viewport is an error. fov must lie in 5..170, the range of the editor's Field of View control, and a value outside it is an error before anything changes. Like that control it sets both the current view FOV and the stored one that a camera reset or the end of a PIE session restores; fov and stored_fov are read back, and an orthographic view ignores them. There is no undo step and no asset is saved, and the previous FOV is not returned. A viewport piloting a camera actor keeps showing that camera's FOV until it stops piloting it. Read it with get_viewport_state (fov); a camera actor's own FOV is set_camera_fov.
//   @param fov - Field of view in degrees, from 5 to 170 (the range of the editor's FOV slider)
//   @returns { applied: bool, requested: number, fov: number, stored_fov: number }
TSharedPtr<FJsonObject> HandleSetViewportFOV(const TSharedPtr<FJsonObject>& Params)
{
    double FovNum = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("fov"), FovNum)) { return Missing; }

    if (!(FovNum >= ViewportControl_MinFOV && FovNum <= ViewportControl_MaxFOV))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("fov %g is outside %g..%g degrees, the range of the editor's Field of View control; the FOV was not changed"),
            FovNum, ViewportControl_MinFOV, ViewportControl_MaxFOV));
    }

    FEditorViewportClient* Client = nullptr;
    if (TSharedPtr<FJsonObject> Error = ViewportControl_RequireClient(Client)) { return Error; }

    // As the level viewport's Field of View control does: FOVAngle is the stored FOV that a camera reset,
    // the end of a PIE session or an actor unlock put back into ViewFOV, the FOV the view draws with now.
    Client->FOVAngle = (float)FovNum;
    Client->ViewFOV = (float)FovNum;
    Client->Invalidate();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetNumberField(TEXT("requested"), FovNum);
    Data->SetNumberField(TEXT("fov"), Client->ViewFOV);
    Data->SetNumberField(TEXT("stored_fov"), Client->FOVAngle);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_near_clip_plane
// ---------------------------------------------------------------------------

// MCP:set_near_clip_plane - Override the near clip plane distance, in cm, of the active level editor viewport, the focused one or else the first; no viewport is an error. A negative distance (-1 by convention) drops the override so the engine near plane, GNearClippingPlane, applies again; a distance of at least 0.001, the smallest the editor's Near View Plane control allows, becomes the override. 0 (a degenerate projection), anything else below 0.001 and a value too large for a float are errors before anything changes. It lasts for this editor session with no undo step and no save, and the previous value is not returned. near_clip_plane is the effective near plane read back and override_active says whether an override is now set. get_viewport_state reads both clip planes (near_clip_plane, far_clip_plane_override); the far plane is set_far_clip_plane.
//   @param distance - Near clip distance in cm, at least 0.001; any negative value (-1 by convention) clears the override and falls back to GNearClippingPlane
//   @returns { applied: bool, requested: number, near_clip_plane: number, override_active: bool }
TSharedPtr<FJsonObject> HandleSetNearClipPlane(const TSharedPtr<FJsonObject>& Params)
{
    double Dist = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("distance"), Dist)) { return Missing; }

    // A negative distance clears the override; any other must give a usable near plane once stored as a
    // float (an infinite one is as degenerate as 0).
    if (Dist >= 0.0 && (Dist < ViewportControl_MinNearClipPlane || Dist > (double)TNumericLimits<float>::Max()))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("distance %g is not a usable near plane: give at least %g cm (the smallest the editor's Near View Plane control allows; 0 makes the projection degenerate) and no more than a float holds, or a negative value (-1) to clear the override; the near plane was not changed"),
            Dist, ViewportControl_MinNearClipPlane));
    }

    FEditorViewportClient* Client = nullptr;
    if (TSharedPtr<FJsonObject> Error = ViewportControl_RequireClient(Client)) { return Error; }

    Client->OverrideNearClipPlane((float)Dist);
    Client->Invalidate();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetNumberField(TEXT("requested"), Dist);
    Data->SetNumberField(TEXT("near_clip_plane"), Client->GetNearClipPlane());
    // override_active: requested >= 0 means override engaged; -1 clears it.
    Data->SetBoolField(TEXT("override_active"), Dist >= 0.0);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_far_clip_plane
// ---------------------------------------------------------------------------

// MCP:set_far_clip_plane - Override the far clip plane distance, in cm, of the active level editor viewport, the focused one or else the first, so objects beyond it are culled from the view; no viewport is an error. 0 or a negative distance drops the override and the default far plane applies again. It lasts for this editor session with no undo step and no save, and the previous value is not returned. far_clip_plane is the stored override read back, which is the distance sent, and override_active is true only for a distance above 0. get_viewport_state reads the near plane together with this override (far_clip_plane_override); the near plane is set_near_clip_plane.
//   @param distance - Far clip distance in cm; 0 or any negative value clears the override and the default far plane applies
//   @returns { applied: bool, requested: number, far_clip_plane: number, override_active: bool }
TSharedPtr<FJsonObject> HandleSetFarClipPlane(const TSharedPtr<FJsonObject>& Params)
{
    double Dist = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("distance"), Dist)) { return Missing; }

    FEditorViewportClient* Client = nullptr;
    if (TSharedPtr<FJsonObject> Error = ViewportControl_RequireClient(Client)) { return Error; }

    Client->OverrideFarClipPlane((float)Dist);
    Client->Invalidate();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetNumberField(TEXT("requested"), Dist);
    Data->SetNumberField(TEXT("far_clip_plane"), Client->GetFarClipPlaneOverride());
    Data->SetBoolField(TEXT("override_active"), Dist > 0.0);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_viewport_exposure
// ---------------------------------------------------------------------------

// MCP:set_viewport_exposure - Set the exposure of the active level editor viewport: fixed at ev100 when fixed is true, automatic eye adaptation when false. Affects the viewport only and saves nothing. Returns applied, fixed and ev100 as read back; get_viewport_state reads them (exposure).
//   @param fixed - True for fixed exposure (use FixedEV100), false for automatic eye-adaptation
//   @param ev100 - EV100 value to use when fixed=true (any float; typical range -3..20)
//   @returns { applied: bool, fixed: bool, ev100: number }
TSharedPtr<FJsonObject> HandleSetViewportExposure(const TSharedPtr<FJsonObject>& Params)
{
    bool bFixed = false;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredBool(Params, TEXT("fixed"), bFixed)) { return Missing; }

    double EV100 = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("ev100"), EV100)) { return Missing; }

    FEditorViewportClient* Client = nullptr;
    if (TSharedPtr<FJsonObject> Error = ViewportControl_RequireClient(Client)) { return Error; }

    Client->ExposureSettings.bFixed = bFixed;
    Client->ExposureSettings.FixedEV100 = (float)EV100;
    Client->Invalidate();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetBoolField(TEXT("fixed"), Client->ExposureSettings.bFixed);
    Data->SetNumberField(TEXT("ev100"), Client->ExposureSettings.FixedEV100);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_game_view
// ---------------------------------------------------------------------------

// MCP:set_game_view - Turn Game View on or off on the active level editor viewport, the focused one or else the first (the view the editor's G key toggles); no viewport is an error. Turning it on swaps the viewport's show flags for the game set, hiding editor-only chrome such as sprites, the transform widget and selection outlines, and keeps the editor set to put back when it is turned off. It sets the given state, so a repeat call changes nothing; it lasts for this editor session with no undo step and nothing saved. in_game_view is read back. Single flags are set_show_flag, render modes set_viewport_view_mode; to run the game use start_play_in_editor.
//   @param enabled - True to enable game view, false to revert to editor view
//   @returns { applied: bool, requested: bool, in_game_view: bool }
TSharedPtr<FJsonObject> HandleSetGameView(const TSharedPtr<FJsonObject>& Params)
{
    bool bEnabled = true;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredBool(Params, TEXT("enabled"), bEnabled)) { return Missing; }

    FEditorViewportClient* Client = nullptr;
    if (TSharedPtr<FJsonObject> Error = ViewportControl_RequireClient(Client)) { return Error; }

    Client->SetGameView(bEnabled);
    Client->Invalidate();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetBoolField(TEXT("requested"), bEnabled);
    Data->SetBoolField(TEXT("in_game_view"), Client->IsInGameView());
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_allow_cinematic_control
// ---------------------------------------------------------------------------

// MCP:set_allow_cinematic_control - Set whether the active level editor viewport (the focused one, else the first) may be possessed by cinematic tools such as Sequencer to preview sequences (FEditorViewportClient::SetAllowCinematicControl). It sets the given value rather than toggling; it lasts for this editor session, nothing is saved or undoable, and no viewport is an error. allows_cinematic_control is read back from the viewport. Read the flag without changing it with get_viewport_state (allows_cinematic_control).
//   @param allow - True to allow cinematic control, false to block it
//   @returns { applied: bool, requested: bool, allows_cinematic_control: bool }
TSharedPtr<FJsonObject> HandleSetAllowCinematicControl(const TSharedPtr<FJsonObject>& Params)
{
    bool bAllow = true;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredBool(Params, TEXT("allow"), bAllow)) { return Missing; }

    FEditorViewportClient* Client = nullptr;
    if (TSharedPtr<FJsonObject> Error = ViewportControl_RequireClient(Client)) { return Error; }

    Client->SetAllowCinematicControl(bAllow);
    Client->Invalidate();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetBoolField(TEXT("requested"), bAllow);
    Data->SetBoolField(TEXT("allows_cinematic_control"), Client->AllowsCinematicControl());
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_view_type
// ---------------------------------------------------------------------------

// MCP:set_view_type - Switch the active level editor viewport, the focused one or else the first, to the perspective camera or one of the six orthographic views; no viewport is an error. type is matched without case, and any other name is an error before anything changes. Switching to an orthographic view unlocks an actor the viewport was piloting, and each projection re-applies the view mode it keeps for itself. It sets the given type, so a repeat call changes nothing; it lasts for this editor session with no undo step and no save. view_type and view_type_index are read back. Read it with get_viewport_state (view_type); render modes are set_viewport_view_mode, the camera position set_viewport_camera.
//   @param type - View type name. One of: Perspective, Top, Bottom, Left, Right, Front, Back. Matched without case.
//   @returns { applied: bool, requested: string, view_type: string, view_type_index: int }
TSharedPtr<FJsonObject> HandleSetViewportType(const TSharedPtr<FJsonObject>& Params)
{
    FString ViewTypeStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("type"), ViewTypeStr, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    ELevelViewportType DesiredType = LVT_MAX;
    const FString N = ViewTypeStr.ToLower();
    if (N == TEXT("perspective"))               { DesiredType = LVT_Perspective; }
    else if (N == TEXT("top"))                  { DesiredType = LVT_OrthoTop; }
    else if (N == TEXT("bottom"))               { DesiredType = LVT_OrthoBottom; }
    else if (N == TEXT("left"))                 { DesiredType = LVT_OrthoLeft; }
    else if (N == TEXT("right"))                { DesiredType = LVT_OrthoRight; }
    else if (N == TEXT("front"))                { DesiredType = LVT_OrthoFront; }
    else if (N == TEXT("back"))                 { DesiredType = LVT_OrthoBack; }
    else
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown view type '%s'. Valid: Perspective, Top, Bottom, Left, Right, Front, Back"), *ViewTypeStr));
    }

    // SetViewportType is declared on FLevelEditorViewportClient, not on FEditorViewportClient.
    FLevelEditorViewportClient* LevelClient = ResolveViewportClient();
    if (!LevelClient)
    {
        return mcp_common::MakeErrorReply(TEXT("No active level editor viewport available"));
    }

    LevelClient->SetViewportType(DesiredType);
    LevelClient->Invalidate();

    const ELevelViewportType ReadBack = LevelClient->GetViewportType();
    const FString ReadBackName = ViewportControl_ViewTypeName(ReadBack);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetStringField(TEXT("requested"), ViewTypeStr);
    Data->SetStringField(TEXT("view_type"), ReadBackName);
    Data->SetNumberField(TEXT("view_type_index"), (int32)ReadBack);
    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("get_viewport_state"), &HandleGetViewportState },
    { TEXT("set_allow_cinematic_control"), &HandleSetAllowCinematicControl },
    { TEXT("set_camera_speed"), &HandleSetCameraSpeed },
    { TEXT("set_far_clip_plane"), &HandleSetFarClipPlane },
    { TEXT("set_game_view"), &HandleSetGameView },
    { TEXT("set_near_clip_plane"), &HandleSetNearClipPlane },
    { TEXT("set_show_flag"), &HandleSetShowFlag },
    { TEXT("set_view_type"), &HandleSetViewportType },
    { TEXT("set_viewport_camera"), &HandleSetViewportCamera },
    { TEXT("set_viewport_exposure"), &HandleSetViewportExposure },
    { TEXT("set_viewport_fov"), &HandleSetViewportFOV },
    { TEXT("set_viewport_realtime"), &HandleSetViewportRealtime },
    { TEXT("set_viewport_view_mode"), &HandleSetViewportViewMode },
};
} // namespace UEAgentViewportControlCommands
