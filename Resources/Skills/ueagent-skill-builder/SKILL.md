---
name: ueagent-skill-builder
description: Turn an Unreal Editor workflow into a reusable skill built on UEAgent's MCP tools. It finds the tools the task needs, rehearses the task once in the editor, checks the result, and writes a SKILL.md with the tool sequence and the pitfalls it met. Use when the user asks to make, capture or automate an Unreal workflow as a skill, or to repeat a task they just did with the tools.
---

# Skill builder

Turns a task the user wants to repeat in the Unreal Editor into a skill: a `SKILL.md` that tells an
agent which UEAgent tools to call, in what order, how to check the result and what goes wrong. The
skill is built from a real run, not from guesses: every step in it has worked once in this editor.
The workflow to capture is the one the user described.

## 1. Pin down the task

Agree with the user, briefly, on:

- **The request**, in the words they will use to ask for it again. These words become the trigger
  phrases of the skill's `description`.
- **What changes between runs**: names, paths, counts, values. These become the skill's inputs.
- **What "done" looks like**, as something a tool can read back: an actor in the level, a Blueprint
  that compiles, an asset with a property set.

One skill does one task. If the request is really two, build two skills.

## 2. Find the tools

UEAgent's tools are the MCP tools of the Unreal Editor's server (named `unreal` when it was connected
from the editor's **MCP** menu). Each client names them its own way: `mcp__unreal__spawn_actor` in
Claude Code, for example. In the skill, write the plain tool name (`spawn_actor`): every client finds
it.

- If your client loads tools on demand, search for them by name or by keywords (`unreal spawn actor`).
- With the compact tool list, search with `find_tools`, read a schema with `describe_tool` and run a
  tool with `call_tool`.
- The tool reference, with search, is at <https://carlxsmg.github.io/UEAgent/tools/>.

Read the whole description of every tool you pick: it names the traps (parameter forms, what it
refuses during Play-in-Editor, what it does not verify). Prefer the most specific tool, use `execute_batch`
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

Save it next to UEAgent's own skills, in the folder the user's client reads:

- Claude Code: `.claude/skills/<name>/SKILL.md` in the project (or `~/.claude/skills/` for every
  project on this machine);
- Codex, Cursor, VS Code, Antigravity CLI and Gemini CLI: `.agents/skills/<name>/SKILL.md` in the
  project.

Ask which client they use if you do not know it. Fill in [template.md](template.md). Rules (the Agent
Skills format, which every one of those clients reads):

- `name`: lowercase letters, digits and hyphens, at most 64 characters, the same as its folder. Do not
  start it with `ueagent-`: that prefix is UEAgent's own.
- `description`: at most 1024 characters; what the skill does and the requests that should trigger it.
- Only the standard frontmatter fields (`name`, `description`, and optionally `license`,
  `compatibility`, `metadata`), so every client reads it the same way.
- Tool and parameter names exactly as the schemas spell them.
- What varies between runs is an input the skill asks for or takes from the request, never a value
  hardcoded from the rehearsal. No absolute paths from this machine except as examples.
- Each step: the tool, the arguments that matter, and what its answer must show.
- A **Verify** section with the read that proves the result, and a **Pitfalls** section with the
  traps from the rehearsal.
- Short: under 150 lines. Long background goes into a separate file next to `SKILL.md`.

## 5. Test the new skill

Run it on a fresh target, not the one used in the rehearsal: ask for it the way the user will (or
invoke it by name, `/<name>` in Claude Code, `$<name>` in Codex), and fix the skill wherever the run
differed from what it says. Then tell the user where it was saved, how to invoke it, and which inputs
it asks for. A client that was already open may need a restart to see a new skill.
