# UEAgent

<img src="assets/logo.svg" alt="UEAgent logo" width="96" align="right">

**The MCP server for Unreal Engine 5, built into the editor.** More than 2,000 tools let Claude Code,
Cursor or any MCP client build in Unreal, with every change checked before it runs and read back
after.

## Get started

1. Download `UEAgent-<version>-UE5.8-Win64.zip` from the
   [latest release](https://github.com/CarlxsMG/UEAgent/releases/latest) and unzip it into
   `<YourProject>/Plugins/`. It is precompiled: no compiler is needed, even in a Blueprint-only
   project.
2. Open the project. The editor's status bar shows **MCP · 55561** once the server is up.
3. Connect your client: [Claude Code, Cursor, VS Code, Windsurf, Claude Desktop, Gemini CLI or
   Codex](clients.md).

## What it does

- **The whole engine.** Blueprints and their graphs, materials, animation, Niagara, levels and World
  Partition, PCG, Sequencer, UMG, GAS, AI, audio, USD and more: see the
  [tool reference](tools/README.md).
- **Verified, not assumed.** Preconditions refuse the calls known to crash or freeze the editor, and
  readback re-reads the state after each call, so a write that did not stick is reported as failed.
- **Safe with your content.** Content under a Read Only Content Root cannot be saved, renamed, moved
  or deleted by any tool, and nothing leaves your machine: the server only listens on 127.0.0.1.
- **For any client.** Clients that cap how many tools a server may list get the
  [compact tool list](clients.md#compact-tool-list).

## Learn more

| | |
|---|---|
| [Examples](examples.md) | Prompts that work well, and the tools they use |
| [FAQ](faq.md) | Versions, safety, cost |
| [Troubleshooting](troubleshooting.md) | When a client cannot connect or a call is refused |
| [Architecture](architecture.md) | How a call travels through the editor, and the contract around it |

UEAgent is open source under the [MIT License](https://github.com/CarlxsMG/UEAgent/blob/main/LICENSE).
Questions go to [Discussions](https://github.com/CarlxsMG/UEAgent/discussions), and the code is on
[GitHub](https://github.com/CarlxsMG/UEAgent).
