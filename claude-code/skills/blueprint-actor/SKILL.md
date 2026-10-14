---
name: blueprint-actor
description: Create a Blueprint actor in Unreal from a description (its components, its event-graph logic, a clean compile and an instance in the level) with UEAgent's tools. Use when the user asks for a new actor Blueprint, such as a door, a pickup, a trigger zone or a turret.
argument-hint: [what the actor is and does]
---

# Blueprint actor

Builds an actor Blueprint from a description: the asset, its components, the event graph, a compile
with no errors, and one instance placed in the level.

The actor to build: $ARGUMENTS

## Steps

1. **Plan** the components (mesh, collision, lights...) and the logic (which events trigger which
   actions). Name the Blueprint `BP_<Thing>`, and ask where it goes if the project has no obvious
   folder for Blueprints.
2. **Create the asset**: `create_blueprint` with `name: BP_<Thing>`, `parent_class: Actor` and `path`
   (a folder such as `/Game/Blueprints`). It is saved to disk at once.
3. **Add the components**: `add_component_to_blueprint` for each one, with `blueprint_name`,
   `component_type` (a short native name such as `StaticMesh`, `Box`, `Sphere`, `PointLight`),
   `component_name`, and optionally `location`, `rotation`, `scale` and `parent_component_name`.
   Send them in one `batch` when there are several.
4. **Build the logic**: `build_blueprint_graph` with `blueprint_name`, `nodes`, `connections` and
   `pin_defaults`, in one call. Read its schema first, and take the pin names from
   `get_function_pins` instead of guessing them.
5. **Compile**: `compile_blueprint` with `return_messages: true`. If it reports errors, read the
   graph with `get_graph_nodes`, fix it, and compile again until `succeeded` is true.
6. **Place it**: `spawn_blueprint_actor` with `blueprint_name`, `actor_name` and a `location` in view
   of the camera.

## Verify

- `compile_blueprint` answers `succeeded: true` with no errors.
- `lint_blueprint_graph` with `blueprint_name` finds nothing: it catches the graph bugs that compile
  clean and only show up in play.
- `get_actors_in_level` lists the placed actor.

## Pitfalls

- On a child Blueprint whose scene root is inherited, read the trap note in
  `add_component_to_blueprint`'s description before you parent a scene component.
- Compiling and saving are refused while Play-in-Editor runs: stop PIE first.

## Undo

Each tool call is one undo step named after the tool ("UEAgent: build_blueprint_graph"). `undo`
reverts the last one; the asset `create_blueprint` saved stays on disk.
