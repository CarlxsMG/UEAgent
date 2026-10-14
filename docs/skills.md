# Claude Code skills

This repository is also a Claude Code plugin marketplace. Its plugin, `ueagent`, adds skills that
drive the editor through UEAgent's tools, and one that writes new skills.

## Install

In Claude Code:

```text
/plugin install ueagent --marketplace CarlxsMG/UEAgent
```

On Claude Code versions before 2.1.275, add the marketplace first:

```text
/plugin marketplace add CarlxsMG/UEAgent
/plugin install ueagent@ueagent
```

The skills need UEAgent's MCP server connected (see [Clients](clients.md)).

## The skills

| Skill | What it does |
|---|---|
| `/ueagent:skill-builder` | Turns a workflow you want to repeat into a new skill |
| `/ueagent:blueprint-actor` | A Blueprint actor from a description: components, graph, a clean compile, and an instance in the level |
| `/ueagent:project-audit` | What is broken and what can go in the project or a folder, most serious first; it changes nothing |
| `/ueagent:level-lighting` | Outdoor lighting (sun, sky, sky light, fog) for noon, a sunset or an overcast sky, and a screenshot |

Type the command, or just ask: Claude loads a skill by itself when a request matches it.

## Building your own skills

`/ueagent:skill-builder <the workflow>` makes a skill out of a task you do often, such as "set up a
pickup with a rotating mesh and a sound" or "import a character and make its AnimBP":

1. It agrees with you on the request, what changes between runs, and what "done" looks like.
2. It finds the tools the task needs, and reads their descriptions for traps.
3. It runs the task once in the editor, on a target you choose, reading every answer: refusals,
   readback warnings and ignored parameters. Each call is one undo step, so a failed attempt is
   rolled back and tried again.
4. It writes the skill, `.claude/skills/<name>/SKILL.md` in the project or
   `~/.claude/skills/<name>/SKILL.md` for every project, with the steps, the check that proves the
   result and the pitfalls it met.
5. It runs the new skill on a fresh target and fixes what differed.

The three other skills in the plugin follow the same format, so they also serve as examples.
