---
name: ueagent-contribute-fix
description: Send a change to UEAgent's code as a pull request to its GitHub repository (CarlxsMG/UEAgent) - a fix made in the copy of the plugin inside an Unreal project (Plugins/UEAgent) or in a clone of the repository - following the repository's rules: the regenerated catalog, the checks, how it was measured, and the PR template. Use when the user wants to contribute a fix or a new tool to UEAgent, send a pull request, or share a change they made to the plugin.
compatibility: Needs git and the GitHub CLI (gh), signed in, to fork the repository and open the pull request; Python 3 for the generators; and the Unreal Engine the change was built with.
---

# Contribute a fix

Takes a change to UEAgent's code and turns it into a pull request on
<https://github.com/CarlxsMG/UEAgent>, the way the repository asks for one in `CONTRIBUTING.md`. A
change that is large or changes how something behaves should have an issue first, so the approach is
agreed before the work (the **ueagent-report-issue** skill opens one).

## 1. Find the change

- **A clone of the repository** (a folder with `.git` and `UEAgent.uplugin`): the change is in its
  working tree. `git status` and `git diff` show it.
- **The plugin inside a project** (`<Project>/Plugins/UEAgent`, installed from a release zip, with no
  `.git`): read `VersionName` in its `UEAgent.uplugin`; the change is the difference between that
  folder and the release tag `v<VersionName>` of the repository.

Show the user the change file by file, and keep only what they want to send: no project files, no
`Binaries/` or `Intermediate/`, no local settings, no tokens.

## 2. A branch on a fork

1. `gh auth status`: signed in? If not, the user runs `gh auth login` themselves.
2. Fork and clone, in a folder the user chooses: `gh repo fork CarlxsMG/UEAgent --clone`.
   In a clone that is already a fork, `git fetch upstream` instead.
3. For a change made in a project copy: `git checkout -b fix/<short-name> v<VersionName>`, copy the
   changed files over, then `git rebase origin/main`, resolving any conflict with the user. In a clone:
   `git checkout -b fix/<short-name> origin/main` (or `upstream/main`).

The repository's text files use LF line endings: keep them so, or the diff becomes every line.

## 3. The repository's rules

From `CONTRIBUTING.md` (read it in the clone: it is the authority):

- Code, comments and docs in English; a comment says what the code does and why.
- A tool is a handler in its family's `.cpp`, with its `// MCP:<name> - <description>` block (one line
  each for the description, every `@param` and the `@returns`) and its row in the family's
  `FRegistrar`.
- Never edit a generated file by hand. After changing the C++, regenerate, in this order:
  `python Tools/integrate_property_handlers.py`, then `codegen_command_catalog.py`,
  `codegen_param_table.py`, `codegen_param_aliases.py`, `codegen_pie_safety.py` and
  `codegen_tool_docs.py`, each with `--write` (all in `Tools/`).
- Then each of them with `--check`, `python Tools/check_doc_links.py`, `ruff check Tools`, and, for a
  change to the skills, `python Tools/check_skills.py`.
- The plugin builds with no warning of its own, for the engine the user has.
- The contract self-tests pass in the editor: **MCP > Contract self-test**, and
  `Automation RunTests UEAgent` in the editor's console.
- A change of behavior says how it was measured: the call before and after, in a headless editor when
  possible.

Run what applies; say plainly which ones could not run here and why.

## 4. Commit, push, open the pull request

- One commit per logical change; the message says what changes for the user, in the present tense.
- `git push -u origin fix/<short-name>`.
- `gh pr create -R CarlxsMG/UEAgent --base main --title "<what it does>" --body-file <file>`, with the
  repository's template filled in: **What and why** (link the issue), **How it was measured**, and the
  checklist (builds with no warning, generators pass `--check`, contract self-tests pass, the
  contribution can be released under the MIT License). Show the user the title and body first.
- Give the user the pull request's link. Never push to `CarlxsMG/UEAgent` itself: always through the
  fork.
