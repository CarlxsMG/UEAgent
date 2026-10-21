# Choosing an MCP server for Unreal Engine

An AI agent can reach the Unreal Editor over MCP in two ways: through a server that runs inside the
editor, or through one that runs outside it and talks to the editor. This page explains what that
choice changes, and lists the questions worth asking of any MCP server for Unreal, with UEAgent's
answers.

## Inside the editor, or outside it

**A server inside the editor** is an editor plugin that serves MCP itself. UEAgent is one.

- A call reaches the editor's objects directly, on the editor's own thread. Nothing has to cross a
  remote channel, and there is no second runtime (Python, Node.js) to install.
- The server can read the state back in the same pass, and wrap a call in one undo step. It knows
  whether Play-in-Editor is running, and it can answer a modal dialog that would otherwise wait for a
  click. All of that needs to be inside the editor.
- It works only while the editor is open. It is built for each engine version.
- The editor runs one call at a time, and a long call keeps it busy until it ends.

**A server outside the editor** is a separate process that the MCP client starts or connects to.

- It can work while the editor is closed, for example to edit files or launch a build. It can be
  restarted on its own and written in any language.
- Each call has to reach the editor through a remote channel: the Remote Control API, Python remote
  execution, or a socket opened by a plugin of its own. It can do only what that channel exposes, and
  many such servers ship an editor plugin anyway.
- Reading back the result, and undoing it, take extra calls through the same channel, when the channel
  allows them at all.

Neither is better for every job. For an agent that builds in an open project, being inside the
editor is what makes it possible to check each call and undo it as one step.

## Questions to ask any server

| Question | Why it matters | UEAgent |
|---|---|---|
| **How much of the engine does it reach?** | An agent stops at the first thing it cannot do, and falls back to guessing or to Python. | Blueprints and their graphs, materials, animation, Niagara, landscapes, World Partition, PCG, Sequencer, UMG and CommonUI, GAS, Behavior Trees, StateTree, EQS, Smart Objects, MetaSounds, USD, Interchange and Play-in-Editor. Browse them in the [tool explorer](explorer.md). |
| **Does `success: true` mean the change happened?** | Editor APIs can report success for a write that did not stick, or that landed in the wrong world. | After each call, readback re-reads the state and reports such a write as failed: see [Answers and refusals](responses.md). |
| **Does it refuse what would crash the editor?** | One bad call can close the editor and lose unsaved work. | Preconditions refuse the calls known to crash or freeze it, and say what to do instead. Calls that are unsafe during Play-in-Editor are refused while it runs, and modal dialogs are answered so that nothing waits for a click. |
| **Can you undo what the agent did?** | Agents make mistakes, and you need to take back one call without losing the rest. | Each call is one undo step, named after the tool, so Ctrl+Z reverts it. A batch is one step, and a transaction can roll back the items that ran before one failed. |
| **Can it change content you do not want changed?** | Marketplace packs and vendored code should stay as they came. | Folders you list as **Read Only Content Roots** cannot be saved, renamed, moved or deleted by any tool: see [Settings](settings.md). |
| **What does it install and run?** | Each extra runtime, process or port is one more thing to set up and to keep running. | The plugin is the server: there is no Python or Node.js process. Releases are precompiled, so it runs in a Blueprint-only project without a compiler. |
| **Does it work with your client?** | Clients differ in transport, and some cap how many tools a server may list. | Streamable HTTP, with one-click setup for Claude Code, Cursor, VS Code and Antigravity CLI, and a command for Codex. For clients with a cap, the [compact tool list](clients.md#compact-tool-list) gives three tools that search, describe and run the rest. |
| **Can several agents share one editor?** | Two agents writing at once can corrupt each other's work. | Every call, from every client, runs on the editor's thread through one queue. The **Activity** panel lists each call with its duration and outcome. |
| **Does the client know what a call will do?** | Clients decide what to ask you to approve from what a tool declares. | Every tool publishes its MCP annotations (read-only, destructive, idempotent), and the descriptions say which of two similar tools to pick. |
| **Who can call it, and what leaves your machine?** | The server can do whatever the editor can. | It listens on 127.0.0.1 only, refuses requests from web pages and from hosts other than the loopback, and can require a token. It makes no network connection of its own: see [Security and privacy](security.md). |
| **Which engines and platforms, and at what cost?** | A server built for another engine version may not load. | Unreal Engine 5.7 and 5.8 on Windows. Free and open source, under the MIT License. |

## What UEAgent does not do

- It does not run without the editor: close the editor and the server stops with it.
- It runs on Windows only, and on Unreal Engine 5.7 and 5.8 only. A few tools need 5.8.
- It does not run two calls at once: a long call, such as a build, keeps the editor busy until it ends.
- It does not limit what a connected agent may do beyond the read-only roots and the refusals:
  connecting an agent gives it the editor, including Python, files and processes.
