# Skills

UEAgent ships agent skills: instructions an AI client loads when a request matches them, so it knows
which tools to call, in what order, and how to check the result. They follow the open
[Agent Skills](https://agentskills.io/specification) format, which Claude Code, Codex, Cursor,
VS Code (GitHub Copilot), Antigravity CLI, Gemini CLI and OpenCode all read.

They come inside the plugin, so they always name the tools of the version you have installed.

## Install

In the editor's status bar, **MCP · 55561 > Skills**:

| Entry | Copies the skills into | For |
|---|---|---|
| **Install the skills for Claude Code** | `.claude/skills/` in the project | Claude Code |
| **Install the skills for Codex, Cursor, VS Code, Antigravity CLI, Gemini CLI and OpenCode** | `.agents/skills/` in the project | every other client |

Install both if the project's team uses several clients. Each copy is stamped with the plugin's
version: after you update UEAgent, the menu says the skills are out of date and offers **Update**.
Only folders UEAgent installed are ever replaced or deleted; a skill of your own is never touched,
and **Remove UEAgent's skills from the project** deletes only UEAgent's.

The skills are files in the project: commit them, and the whole team has them. A client that is
already open may need a restart to see new skills.

Cursor and VS Code read both folders: with both installed, they may list each skill twice. Install
only the `.agents/skills` set if nobody on the team uses Claude Code.

Without the editor's menu, the console does the same: `mcp.skills.status`, `mcp.skills.install
claude|agents|all` and `mcp.skills.remove claude|agents|all`.

The skills need UEAgent's MCP server connected to the client: see [Clients](clients.md).

## The skills

| Skill | What it does |
|---|---|
| `ueagent-tool-map` | Which core tool, pack or domain covers a task, and the order a task calls them in: find, read, change, check, compile, save |
| `ueagent-skill-builder` | Turns a workflow you want to repeat into a new skill |
| `ueagent-blueprint-actor` | A Blueprint actor from a description: components, graph, a clean compile, and an instance in the level |
| `ueagent-project-audit` | What is broken and what can go in the project or a folder, most serious first; it changes nothing |
| `ueagent-level-lighting` | Outdoor lighting (sun, sky, sky light, fog) for noon, a sunset or an overcast sky, and a screenshot |
| `ueagent-report-issue` | Reports a UEAgent bug, or asks for a tool, as an issue on GitHub: it gathers the versions, the call and the log, and shows you the issue before it opens it |
| `ueagent-contribute-fix` | Turns a change you made to the plugin's code into a pull request, following the repository's rules |

Just ask, and the client loads the skill that matches. To call one by name: `/ueagent-skill-builder`
in Claude Code, `$ueagent-skill-builder` in Codex, or the skill picker of your client.

The tables of `ueagent-tool-map` (the core tools, the packs and the domains) are generated from the tool
catalog, so they always match the tools of the installed version.

## Without installing them: prompts and resources

The server also serves the skills over MCP, from the same files, so a client reaches them without
copying anything into the project:

- **Prompts**: one per skill, named like it, with an optional `request` (what you want done). A client
  offers them to you, usually as slash commands: `/mcp__unreal__ueagent-blueprint-actor` in Claude
  Code. The prompt sends the skill to the agent, with your request and the skill's other files.
- **Resources**: `ueagent://guide` is the tool map, `ueagent://skills/<name>` each other skill, and
  `ueagent://skills/<name>/<file>` a file of one. In Claude Code, mention one with
  `@unreal:ueagent://guide`; some clients let the agent read them by itself.

The server's instructions point every agent to `ueagent://guide`. Which of the two a client supports
varies: Claude Code, Cursor, VS Code, Gemini CLI and Zed list prompts; Codex, OpenCode and Windsurf do
not, and read the installed skills instead.

## Building your own skills

Ask `ueagent-skill-builder` to make a skill out of a task you do often, such as "set up a pickup with
a rotating mesh and a sound" or "import a character and make its AnimBP":

1. It agrees with you on the request, what changes between runs, and what "done" looks like.
2. It finds the tools the task needs, and reads their descriptions for traps.
3. It runs the task once in the editor, on a target you choose, reading every answer: refusals,
   readback warnings and ignored parameters. Each call is one undo step, so a failed attempt is
   rolled back and tried again.
4. It writes the skill next to UEAgent's, in `.claude/skills/<name>/` or `.agents/skills/<name>/`,
   with the steps, the check that proves the result and the pitfalls it met.
5. It runs the new skill on a fresh target and fixes what differed.

Your skills keep their own names: the `ueagent-` prefix belongs to the skills UEAgent installs.

## Contributing

`ueagent-report-issue` and `ueagent-contribute-fix` follow the repository's own rules, in
[CONTRIBUTING.md](https://github.com/CarlxsMG/UEAgent/blob/main/CONTRIBUTING.md): an issue with the
call, the versions and the log; and a pull request with the catalog regenerated, the checks passed and
how the change was measured. A security problem is reported privately, never in an issue.

## Coming from the Claude Code plugin

Before 2.2.0 the skills came from a Claude Code plugin marketplace in this repository, which always
followed the latest code. It is retired. Remove it, then install the skills from the editor's menu:

```text
/plugin uninstall ueagent@ueagent
/plugin marketplace remove ueagent
```
