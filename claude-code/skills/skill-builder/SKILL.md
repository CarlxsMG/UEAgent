---
name: skill-builder
description: Turn an Unreal Editor workflow into a reusable skill built on UEAgent's MCP tools. It finds the tools the task needs, rehearses the task once in the editor, checks the result, and writes a SKILL.md with the tool sequence and the pitfalls it met. Use when the user asks to make, capture or automate an Unreal workflow as a skill, or to repeat a task they just did with the tools.
argument-hint: [the workflow to turn into a skill]
---

# Skill builder

Turns a task the user wants to repeat in the Unreal Editor into a skill: a `SKILL.md` that tells
Claude which UEAgent tools to call, in what order, how to check the result and what goes wrong. The
skill is built from a real run, not from guesses: every step in it has worked once in this editor.

The workflow to capture: $ARGUMENTS

## 1. Pin down the task

Agree with the user, briefly, on:

- **The request**, in the words they will use to ask for it again. These words become the trigger
  phrases of the skill's `description`.
- **What changes between runs**: names, paths, counts, values. These become the skill's inputs.
- **What "done" looks like**, as something a tool can read back: an actor in the level, a Blueprint
  that compiles, an asset with a property set.

One skill does one task. If the request is really two, build two skills.

## 2. Find the tools

UEAgent's tools are this session's MCP tools, named `mcp__<server>__<tool>`
(`mcp__unreal__spawn_actor` when the project was connected with the editor's **Connect** button).

- If the tools are deferred, load them with ToolSearch: by name (`select:mcp__unreal__spawn_actor`) or
  by keywords (`unreal spawn actor`).
- With the compact tool list, search with `find_tools` and read a schema with `describe_tool`.
- The tool reference, with search, is at <https://carlxsmg.github.io/UEAgent/tools/>.

Read the whole description of every tool you pick: it names the traps (parameter forms, what it
refuses during Play-in-Editor, what it does not verify). Prefer the most specific tool, use `batch`
for steps that do not depend on each other's answers, and keep `execute_python` for what no tool does.

## 3. Rehearse it in the editor

Run the sequence once, on a test target the user agrees to (a test level, a scratch folder), and
read every answer:

- `success: false`, or a refusal (`isError`): follow what the message says to do instead.
- A `readback` whose `verdict` is not `ok`: the call did not do what it reported; its `note` says why.
- `unknown_params`: a parameter the tool ignored, so its name is wrong.

Each tool call is one undo step ("UEAgent: <tool>"): `undo` reverts the last one, so a failed attempt
can be rolled back and tried again. Confirm the end state with a read tool (`get_*`, `list_*`,
`find_*`), not with the success flags. Write down every trap you hit and how you got past it.

## 4. Write the skill

Ask where to save it:

- this project: `.claude/skills/<name>/SKILL.md`
- every project on this machine: `~/.claude/skills/<name>/SKILL.md`

Fill in [template.md](template.md). Rules:

- `name` in kebab-case; a `description` that says what the skill does and lists the requests that
  should trigger it.
- Tool and parameter names exactly as the schemas spell them.
- What varies between runs is an input (`arguments`, used as `$name` in the body), never a value
  hardcoded from the rehearsal. No absolute paths from this machine except as examples.
- Each step: the tool, the arguments that matter, and what its answer must show.
- A **Verify** section with the read that proves the result, and a **Pitfalls** section with the
  traps from the rehearsal.
- Short: under 150 lines. Long background goes into a separate file next to `SKILL.md`.

## 5. Test the new skill

Invoke it (`/<name>`) on a fresh target, not the one used in the rehearsal, and fix the skill
wherever the run differed from what it says. Then tell the user where it was saved, how to invoke
it, and which inputs it asks for.
