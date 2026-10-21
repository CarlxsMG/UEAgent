---
name: ueagent-level-lighting
description: Light the current Unreal level as an outdoor scene (a sun, a sky atmosphere, a sky light and height fog) with UEAgent's MCP tools, then show the result in a screenshot. Use when the user asks to light a level, set up daylight or a sunset, or add a sky.
---

# Level lighting

Sets up outdoor lighting in the level open in the editor: a directional sun, a sky atmosphere, a sky
light and exponential height fog, tuned to the look the user asked for (noon, sunset or overcast;
noon if they named none), and a screenshot to show it.

UEAgent's tools are the MCP tools of the Unreal Editor's server (named `unreal` when it was connected
from the editor's **MCP** menu). Your client may show them with a prefix, such as
`mcp__unreal__spawn_actor`. With the compact tool list, run them through `call_tool`.

## Steps

1. **See what is there**: `get_actors_in_level`. If the level already has a DirectionalLight, a
   SkyAtmosphere, a SkyLight or an ExponentialHeightFog, reuse it instead of adding a second one.
2. **The sun**: `spawn_actor` with `type: DirectionalLight`, `name: Sun`, `rotation` in the object
   form, `{"pitch": -40, "yaw": 30, "roll": 0}` for noon or `{"pitch": -8, "yaw": 30, "roll": 0}` for
   a sunset, and `intensity` (in lux; around 10 for full daylight, lower for a sunset or an overcast
   sky).
3. **The sky**: `spawn_actor` with `type: SkyAtmosphere` and `name: Sky`.
4. **Sky light**: `spawn_actor` with `type: SkyLight` and `name: SkyLight`, then `call_function` with
   `object_path: SkyLight.SkyLightComponent` and `function_name: RecaptureSky`, so it picks up the new
   sky.
5. **Fog**: `spawn_actor` with `type: ExponentialHeightFog`, `name: Fog` and `properties`
   `{"ExponentialHeightFogComponent.FogDensity": 0.02}` (0.02 for clear air, around 0.05 for overcast).
6. **Tune it later**: `set_properties` on `Sun.DirectionalLightComponent`, `Fog.ExponentialHeightFogComponent`
   or `SkyLight.SkyLightComponent`, with the engine's property names (`Intensity`, `FogDensity`...).
7. **Show it**: `capture_high_res_screenshot`, and tell the user the file it wrote.

## Verify

- `get_actors_in_level` lists one of each: the Sun, the SkyAtmosphere, the SkyLight and the
  ExponentialHeightFog.
- The screenshot shows a lit scene, not a black sky.

## Pitfalls

- `spawn_actor`'s `rotation` as an array is `[pitch, yaw, roll]`: its third value is the roll, not the
  yaw. The object form avoids that mistake.
- Nothing is saved: `save_level` writes the level when the user is happy with it.

## Undo

Each tool call is one undo step named after the tool ("UEAgent: spawn_actor"); `undo` reverts the
last one.
