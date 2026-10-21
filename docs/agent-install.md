# Install with your agent

Your AI agent can install UEAgent for you. Open Claude Code, Cursor, Codex or another agent in your
project's folder, and paste:

```text
Install the UEAgent plugin in this Unreal Engine project and connect yourself to it, following https://raw.githubusercontent.com/CarlxsMG/UEAgent/main/llms-install.md
```

The agent then does what [Getting started](getting-started.md) describes:

1. It finds the project and its engine version.
2. It downloads the latest release for that engine, and checks it against the release's SHA-256.
3. It unzips it into `Plugins`. An older copy goes to `Saved/UEAgentUpdate/Backup`, and a git clone is left alone.
4. It opens the project in the editor.
5. It connects itself to the server and installs its skills.

It tells you what it is about to do and asks before it changes anything.

The one step left to you is at the end: reload your agent's MCP servers so the `unreal` tools appear. In
Claude Code, start a new session in the project (it asks once to approve the project's server) or run
`/mcp`.

The agent follows [llms-install.md](https://github.com/CarlxsMG/UEAgent/blob/main/llms-install.md), in
the repository: instructions written for an agent, with the PowerShell it runs at each step.
