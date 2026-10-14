# Security policy

## Reporting a vulnerability

Please do not open a public issue. Report it privately on this repository: **Security > Report a
vulnerability**. Include the UEAgent and Unreal Engine versions, the steps to reproduce and the
impact. The report is credited in the fix unless you prefer otherwise.

## Scope

The MCP server is local by design: it listens on 127.0.0.1 and checks the `Host` and `Origin` of
every request. In scope:

- anything that lets a web page, another user or a remote host call the server;
- a way around the auth token;
- a tool that saves, renames, moves or deletes content under a Read Only Content Root.

Out of scope: what an agent you connected does with the tools. They act with the editor's rights by
design (see the Security section of the [README](README.md)).
