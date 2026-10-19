---
name: level-lighting
description: Light the current Unreal level as an outdoor scene (a sun, a sky atmosphere, a sky light and height fog) with UEAgent's tools, then show the result in a screenshot. Use when the user asks to light a level, set up daylight or a sunset, or add a sky.
argument-hint: [noon | sunset | overcast]
---

# Level lighting

Sets up outdoor lighting in the level open in the editor: a directional sun, a sky atmosphere, a sky
light and exponential height fog, tuned to the time of day asked for, and a screenshot to show it.

The look to aim for (noon if none is given): $ARGUMENTS

## Steps

1. **See what is there**: `get_actors_in_level`. If the level already has a DirectionalLight, a
   SkyAtmosphere, a SkyLight or an ExponentialHeightFog, reuse it instead of adding a second one.
2. **The sun**: `spawn_actor` with `type: DirectionalLight`, `name: Sun` and `rotation` in the object
   form, `{"pitch": -40, "yaw": 30, "roll": 0}` for noon or `{"pitch": -8, "yaw": 30, "roll": 0}` for
   a sunset.
3. **Its strength**: `set_light_intensity` with `light_name: Sun` and `intensity` (in lux; around 10
   for full daylight, lower for a sunset or an overcast sky).
4. **The sky**: `spawn_sky_atmosphere`.
5. **Sky light**: `spawn_sky_light` with `recapture: true`, so it picks up the new sky.
6. **Fog**: `spawn_exponential_height_fog` with `fog_density` (0.02 for clear air, around 0.05 for
   overcast).
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
