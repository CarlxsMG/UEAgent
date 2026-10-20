# Settings

What UEAgent lets you set, in the editor, on the command line and in the environment, and its console
commands.

## In the editor

<!-- include: README.md#settings -->
The settings are listed in the [README](../README.md#settings).
<!-- /include -->

## Command line and environment

| | |
|---|---|
| `-McpHttpPort=N` | On the editor's command line: the server's port, instead of **HTTP port** |
| `UEAGENT_TOOLS=compact` | In the environment the editor starts in: every client gets the [compact tool list](clients.md#compact-tool-list), and `?tools=full` gives one client the whole list back |
| `UEAGENT_TOKEN` | The token the `.mcp.json` entry written by **Connect Claude Code** sends, as `Authorization: Bearer ${UEAGENT_TOKEN}`: give it the value of **Auth token** |

## Console commands

In the editor's console, or through the `execute_console_command` tool:

<!-- include: README.md#console-commands -->
The console commands are listed in the [README](../README.md#console-commands).
<!-- /include -->
