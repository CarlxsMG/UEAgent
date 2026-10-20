---
name: ueagent-report-issue
description: Report a problem with UEAgent, or ask for a feature, as an issue on its GitHub repository (CarlxsMG/UEAgent) - a tool that failed, answered success without doing the work, refused a valid call, or closed or froze the editor; or something no tool does yet. It gathers the versions, the call and the editor log, shows the user the full issue, and opens it only once they agree. Use when the user wants to report a UEAgent bug, file an issue, or request a tool.
compatibility: Opens the issue with the GitHub CLI (gh) when it is installed and signed in; otherwise gives a link that opens the form already filled.
---

# Report an issue

Turns a problem with UEAgent, or a missing tool, into an issue on <https://github.com/CarlxsMG/UEAgent>
with what the maintainers need to reproduce it. The issue is **public**: nothing goes in it that the
user has not read and agreed to.

A **security** problem (another program or a web page able to call the server, a way around the auth
token, a write under a read-only content root) never goes in a public issue: point the user to
<https://github.com/CarlxsMG/UEAgent/security/advisories/new> and stop.

## 1. Bug or feature?

- **Bug**: a tool failed, answered `success: true` without doing the work (its `readback` or a read
  tool shows the truth), refused a call that should work, or closed or froze the editor.
- **Feature**: something the user wants an agent to do in Unreal that no tool does. Search first with
  `find_tools` (or the reference at <https://carlxsmg.github.io/UEAgent/tools/>): the tool may exist
  under another name.

## 2. Gather the facts (bug)

- **The tool and the call**: its name, the parameters sent and the answer, from this conversation.
  If the call came from someone else, ask for it; the editor's **MCP > Activity...** window lists the
  last calls.
- **Versions**:
  - UEAgent: `VersionName` in `UEAgent.uplugin` (in the project's `Plugins/UEAgent/`, or in the
    engine's plugins);
  - Unreal Engine: `create_project_overview` reports it, or the engine folder's name;
  - the operating system and this MCP client.
- **The editor log**: `get_output_log_lines` with `filter` set to the tool's name or the error text;
  or the lines around the call in `Saved/Logs/<Project>.log`. After a crash, the newest folder in
  `Saved/Crashes/`: its `CrashContext.runtime-xml` holds the call stack.
- **The smallest reproduction**: the fewest calls, on a new empty level or a blank project if
  possible, that show the problem. Try it once if it is safe to; say whether it reproduced.

Remove what the user would not publish: absolute paths with their name, project or asset names they
want private, tokens. Replace them with placeholders.

## 3. Avoid a duplicate

`gh issue list -R CarlxsMG/UEAgent --state all --search "<tool name>"`, or search the issues page.
When one already covers it, offer to add the new facts there as a comment instead.

## 4. Write it and show it

Follow the repository's forms:

- **Bug** (label `bug`): *Tool*; *The call* (the parameters, as JSON); *What happened, and what you
  expected*; *Editor log*; *Versions* (`UEAgent <version>, UE <version>, <operating system>, <client>`).
- **Feature** (label `enhancement`): *What you want to do* (the task, as you would ask an agent);
  *Closest tools today* (what almost does it, and what is missing); *Engine API, if you know it*.

The title says the problem in a line: `set_actor_transform answers success but the actor does not move`.
Show the user the whole issue, title and body, and change it until they approve it.

## 5. Open it

- With the GitHub CLI signed in (`gh auth status`): write the body to a file and run
  `gh issue create -R CarlxsMG/UEAgent --title "<title>" --body-file <file> --label bug` (or
  `--label enhancement`). Give the user the link it prints.
- Without it: give a link that opens the form filled in, with each value URL-encoded:
  `https://github.com/CarlxsMG/UEAgent/issues/new?template=bug_report.yml&title=<title>&tool=<tool>&call=<call>&what-happened=<text>&log=<log>&versions=<versions>`
  (a feature: `template=feature_request.yml` with `goal`, `closest` and `engine`). Keep the link under
  about 8000 characters: put a long log in the issue by hand after it opens.

A question about how to do something is not an issue: it goes to
<https://github.com/CarlxsMG/UEAgent/discussions/categories/q-a>.
