#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EditorViewportClient.h"
#include "Settings/EditorViewportSettings.h"
#include "LevelEditorViewport.h"
#include "ShowFlags.h"
#include "Engine/EngineBaseTypes.h"
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

    /** Map a string view-mode name to EViewModeIndex. Returns false if unknown. */
    bool ViewModeNameToIndex(const FString& Name, EViewModeIndex& OutIndex)
    {
        const FString N = Name.ToLower();
        if (N == TEXT("lit"))                 { OutIndex = VMI_Lit; return true; }
        if (N == TEXT("unlit"))               { OutIndex = VMI_Unlit; return true; }
        if (N == TEXT("wireframe"))           { OutIndex = VMI_BrushWireframe; return true; }
        if (N == TEXT("brushwireframe"))      { OutIndex = VMI_BrushWireframe; return true; }
        if (N == TEXT("csgwireframe"))        { OutIndex = VMI_Wireframe; return true; }
        if (N == TEXT("detaillighting"))      { OutIndex = VMI_Lit_DetailLighting; return true; }
        if (N == TEXT("lightingonly"))        { OutIndex = VMI_LightingOnly; return true; }
        if (N == TEXT("lightcomplexity"))     { OutIndex = VMI_LightComplexity; return true; }
        if (N == TEXT("shadercomplexity"))    { OutIndex = VMI_ShaderComplexity; return true; }
        if (N == TEXT("lightmapdensity"))     { OutIndex = VMI_LightmapDensity; return true; }
        if (N == TEXT("reflections"))         { OutIndex = VMI_ReflectionOverride; return true; }
        if (N == TEXT("reflectionoverride"))  { OutIndex = VMI_ReflectionOverride; return true; }
        return false;
    }

    /** Map an EViewModeIndex back to the canonical string used by this API. */
    FString ViewModeIndexToName(EViewModeIndex Index)
    {
        switch (Index)
        {
        case VMI_Lit:                 return TEXT("Lit");
        case VMI_Unlit:               return TEXT("Unlit");
        case VMI_BrushWireframe:      return TEXT("Wireframe");
        case VMI_Wireframe:           return TEXT("BrushWireframe");
        case VMI_Lit_DetailLighting:  return TEXT("DetailLighting");
        case VMI_LightingOnly:        return TEXT("LightingOnly");
        case VMI_LightComplexity:     return TEXT("LightComplexity");
        case VMI_ShaderComplexity:    return TEXT("ShaderComplexity");
        case VMI_LightmapDensity:     return TEXT("LightmapDensity");
        case VMI_ReflectionOverride:  return TEXT("Reflections");
        default:                      return FString::Printf(TEXT("ViewMode_%d"), (int32)Index);
        }
    }

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

// MCP:set_viewport_view_mode - Set the rendering view mode of the active editor viewport
//   @param view_mode - One of Lit, Unlit, Wireframe, DetailLighting, LightingOnly, BrushWireframe, ShaderComplexity, LightComplexity, LightmapDensity, Reflections
//   @returns { applied: bool, requested: string, view_mode_index: int, view_mode: string }
TSharedPtr<FJsonObject> HandleSetViewportViewMode(const TSharedPtr<FJsonObject>& Params)
{
    FString ViewModeStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("view_mode"), ViewModeStr, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    EViewModeIndex DesiredIndex = VMI_Lit;
    if (!ViewModeNameToIndex(ViewModeStr, DesiredIndex))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown view_mode '%s'. Valid: Lit, Unlit, Wireframe, DetailLighting, LightingOnly, BrushWireframe, ShaderComplexity, LightComplexity, LightmapDensity, Reflections"), *ViewModeStr));
    }

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

// MCP:set_show_flag - Toggle a single engine show flag on the active editor viewport
//   @param flag_name - Engine show flag name, e.g. Grid, Bounds, Collision, Wireframe, Lighting
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

// MCP:set_camera_speed - Set the editor viewport camera movement speed (float multiplier)
//   @param speed - Camera speed (positive float; the editor clamps to its valid range)
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

// MCP:set_viewport_camera - Move the active editor viewport camera to a new location and or rotation
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

// MCP:get_viewport_state - Read the current state of the active editor viewport
//   @returns { realtime: bool, view_mode_index: int, view_mode: string, camera_speed: int, location: [number], rotation: [number], show_flags: object }
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
    return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// FOV, clip planes, exposure, game view, cinematic control, view type.
// All headless-friendly except set_viewport_layout (Slate-dependent — omitted).

// ---------------------------------------------------------------------------
// set_viewport_fov
// ---------------------------------------------------------------------------

// MCP:set_viewport_fov - Set the perspective FOV (in degrees) of the active editor viewport
//   @param fov - Field of view in degrees (positive; clamped to 5..170 by editor)
//   @returns { applied: bool, requested: number, fov: number }
TSharedPtr<FJsonObject> HandleSetViewportFOV(const TSharedPtr<FJsonObject>& Params)
{
    double FovNum = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("fov"), FovNum)) { return Missing; }

    FEditorViewportClient* Client = nullptr;
    if (TSharedPtr<FJsonObject> Error = ViewportControl_RequireClient(Client)) { return Error; }

    Client->ViewFOV = (float)FovNum;
    Client->Invalidate();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetNumberField(TEXT("requested"), FovNum);
    Data->SetNumberField(TEXT("fov"), Client->ViewFOV);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_viewport_fov
// ---------------------------------------------------------------------------

// MCP:get_viewport_fov - Read the current perspective FOV (in degrees) of the active editor viewport
//   @returns { fov: number }
TSharedPtr<FJsonObject> HandleGetViewportFOV(const TSharedPtr<FJsonObject>& Params)
{
    FEditorViewportClient* Client = nullptr;
    if (TSharedPtr<FJsonObject> Error = ViewportControl_RequireClient(Client)) { return Error; }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("fov"), Client->ViewFOV);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_near_clip_plane
// ---------------------------------------------------------------------------

// MCP:set_near_clip_plane - Override the near clip plane distance on the active editor viewport
//   @param distance - Near clip distance (in cm). Pass -1 to clear the override (use GNearClippingPlane).
//   @returns { applied: bool, requested: number, near_clip_plane: number, override_active: bool }
TSharedPtr<FJsonObject> HandleSetNearClipPlane(const TSharedPtr<FJsonObject>& Params)
{
    double Dist = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("distance"), Dist)) { return Missing; }

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
// get_near_clip_plane
// ---------------------------------------------------------------------------

// MCP:get_near_clip_plane - Read the near clip plane (and far clip override) of the active editor viewport
//   @returns { near_clip_plane: number, far_clip_plane_override: number }
TSharedPtr<FJsonObject> HandleGetNearClipPlane(const TSharedPtr<FJsonObject>& Params)
{
    FEditorViewportClient* Client = nullptr;
    if (TSharedPtr<FJsonObject> Error = ViewportControl_RequireClient(Client)) { return Error; }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("near_clip_plane"), Client->GetNearClipPlane());
    // Far override is < 0 when no override is engaged (falls back to GFarClippingPlane).
    Data->SetNumberField(TEXT("far_clip_plane_override"), Client->GetFarClipPlaneOverride());
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_far_clip_plane
// ---------------------------------------------------------------------------

// MCP:set_far_clip_plane - Override the far clip plane distance on the active editor viewport
//   @param distance - Far clip distance (in cm). Pass 0 to clear the override (use GFarClippingPlane).
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

// MCP:set_viewport_exposure - Set fixed-exposure settings on the active editor viewport
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
// get_viewport_exposure
// ---------------------------------------------------------------------------

// MCP:get_viewport_exposure - Read the current exposure settings of the active editor viewport
//   @returns { fixed: bool, ev100: number }
TSharedPtr<FJsonObject> HandleGetViewportExposure(const TSharedPtr<FJsonObject>& Params)
{
    FEditorViewportClient* Client = nullptr;
    if (TSharedPtr<FJsonObject> Error = ViewportControl_RequireClient(Client)) { return Error; }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("fixed"), Client->ExposureSettings.bFixed);
    Data->SetNumberField(TEXT("ev100"), Client->ExposureSettings.FixedEV100);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_game_view
// ---------------------------------------------------------------------------

// MCP:set_game_view - Toggle "Game View" mode on the active editor viewport (hides editor-only chrome)
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

// MCP:set_allow_cinematic_control - Toggle cinematic-control on the active editor viewport (Sequencer overrride)
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
// get_allow_cinematic_control
// ---------------------------------------------------------------------------

// MCP:get_allow_cinematic_control - Read whether the active editor viewport allows cinematic control (Sequencer possession)
//   @returns { allows_cinematic_control: bool }
TSharedPtr<FJsonObject> HandleGetAllowCinematicControl(const TSharedPtr<FJsonObject>& Params)
{
    FEditorViewportClient* Client = nullptr;
    if (TSharedPtr<FJsonObject> Error = ViewportControl_RequireClient(Client)) { return Error; }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("allows_cinematic_control"), Client->AllowsCinematicControl());
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_view_type
// ---------------------------------------------------------------------------

// MCP:set_view_type - Set the active editor viewport type (perspective / top / bottom / left / right / front / back)
//   @param type - View type name. One of: Perspective, Top, Bottom, Left, Right, Front, Back.
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

// ---------------------------------------------------------------------------
// get_view_type
// ---------------------------------------------------------------------------

// MCP:get_view_type - Read the current view type of the active level editor viewport
//   @returns { view_type: string, view_type_index: int }
TSharedPtr<FJsonObject> HandleGetViewportType(const TSharedPtr<FJsonObject>& Params)
{
    FLevelEditorViewportClient* LevelClient = ResolveViewportClient();
    if (!LevelClient)
    {
        return mcp_common::MakeErrorReply(TEXT("No active level editor viewport available"));
    }

    const ELevelViewportType VT = LevelClient->GetViewportType();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("view_type"), ViewportControl_ViewTypeName(VT));
    Data->SetNumberField(TEXT("view_type_index"), (int32)VT);
    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("get_allow_cinematic_control"), &HandleGetAllowCinematicControl },
    { TEXT("get_near_clip_plane"), &HandleGetNearClipPlane },
    { TEXT("get_view_type"), &HandleGetViewportType },
    { TEXT("get_viewport_exposure"), &HandleGetViewportExposure },
    { TEXT("get_viewport_fov"), &HandleGetViewportFOV },
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
