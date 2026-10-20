---
name: <kebab-case-name>
description: <What it does, in one sentence>. Use when <the requests that should trigger it>.
---

# <Title>

<One paragraph: what this skill makes in the editor, and the result to expect.>

UEAgent's tools are the MCP tools of the Unreal Editor's server (named `unreal` when it was connected
from the editor's **MCP** menu). Your client may show them with a prefix. With the compact tool list,
run them through `call_tool`.

## Inputs

Take them from the user's request; ask for the ones it does not give.

- **<input>**: <what it is, with an example>
- **<input>**: <...>

## Steps

1. **<What this step does>**: `<tool_name>` with `<param>: <value>` and `<param>`: the <input>.
   Check: <what the answer must show>.
2. **<...>**: `<tool_name>` ...

## Verify

- `<read tool>` with `<params>`: <what it must return>.

## Pitfalls

- <A refusal or a readback note met while building this skill, and what to do instead.>

## Undo

Each tool call is one undo step named after the tool ("UEAgent: <tool>"). `undo` reverts the last
one, and `get_undo_state` names what it will revert. Undo restores the editor's state: a file a step
saved stays saved.
