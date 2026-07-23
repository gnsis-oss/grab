# grab-mcp — Model Context Protocol server (specification)

This directory specifies `grab-mcp`, a small [MCP](https://modelcontextprotocol.io)
server that exposes the grab CLI as typed, callable tools. MCP is client-neutral:
one server works with Claude Code, Claude Desktop, Codex, Cursor, and any other
MCP host. This is the design; the server binary is **not built yet**.

- Tool catalog (authoritative, machine-readable): [`tools.json`](./tools.json)
- CLI it wraps: the `grab` verbs in `include/grab/command_descriptor.hpp`

## Why MCP

A skill (see `integrations/claude/skills/grab/SKILL.md`) teaches an agent *when*
to reach for grab and *how* the CLI is shaped. MCP is the other half: it lets the
agent *call* grab directly. Each tool advertises a name, a description,
`annotations`, and a JSON input schema; a host reads those (plus the server's
`instructions`) to pick and populate a tool — so the catalog **is** the
feature-discovery surface.

## Transport & contract

- **Transport:** stdio. grab drives a live X11 display, so the server must run
  locally on the machine with the desktop session (or a virtual `Xvfb`). A
  hosted HTTP/SSE server cannot reach the user's screen and is not appropriate.
- **Implementation:** a thin adapter. Each tool call invokes `grab <verb>` via an
  explicit **argv array** (never a shell string) using a per-tool adapter that
  renames arguments, enforces mutual exclusion, and converts values — do not
  generate argv from the catalog's `cli` string. Only `doctor`, `windows`, and
  `watch status` emit JSON today; parse text for the rest (or add `--json`
  upstream). No desktop logic lives in the server — grab owns all of it.
- **Errors:** surface grab's non-zero exit and stderr via `isError: true`; do not
  fabricate success. Return `capture` results as MCP **image content**, not just
  a path, so the host can see the screenshot.

## Safety model

Each tool in `tools.json` carries standard MCP `annotations` plus a grab-native
`x-grab-category`:

- **`annotations`** — MCP `ToolAnnotations` (`readOnlyHint`, `destructiveHint`,
  `idempotentHint`). Publish these so a host like Codex can drive approval modes.
  `readOnlyHint` follows MCP semantics: `capture_screen`, `watch_capture`, and
  `batch` **write files**, so they are *not* `readOnlyHint: true` even though
  they never touch the desktop.
- **`x-grab-category`** — grab's own classification, and the thing a `--read-only`
  mode / allowlist should gate on:
  - `observe` — `doctor`, `list_windows`, `compare_images` (no desktop change).
  - `capture` — `capture_screen`, `watch_capture`, `batch` (write image files).
  - `window-control` — `focus_window`, `place_window` (mutate the desktop; idempotent).
  - `input-synthesis` — `click`, `type_text`, `key`, `drag`, `drag_curve` (synthesize input).

A server SHOULD support `--read-only`, registering only the `observe`+`capture`
tools, and MAY gate `window-control`+`input-synthesis` behind an explicit
allowlist. Enforce this **server-side** — the host's shell sandbox is not a
desktop-control boundary.

## Wiring it into a host

Once `grab-mcp` is built and on `PATH` (or referenced by absolute path):

### Claude Code — project `.mcp.json`

```json
{
  "mcpServers": {
    "grab": {
      "type": "stdio",
      "command": "grab-mcp",
      "args": [],
      "env": { "GRAB_BIN": "grab" }
    }
  }
}
```

### Claude Code — bundled in the plugin

Add this block to `integrations/claude/.claude-plugin/plugin.json` so installing
the plugin also wires the tools (ship the binary under the plugin's `bin/`):

```json
{
  "mcpServers": {
    "grab": {
      "type": "stdio",
      "command": "${CLAUDE_PLUGIN_ROOT}/bin/grab-mcp",
      "args": []
    }
  }
}
```

It is intentionally left out of the committed `plugin.json` for now, so the
skill-only plugin installs cleanly before the server exists.

### Codex — `~/.codex/config.toml`

`command` alone selects the stdio transport — do **not** add a `type`/`transport`
key (that is Claude's `.mcp.json` shape, not Codex's). Codex launches stdio
servers with a **filtered environment** that does not forward `DISPLAY` or
`XAUTHORITY`, so a desktop tool must forward them explicitly or it starts blind
to the screen:

```toml
[mcp_servers.grab]
command = "/absolute/path/to/grab-mcp"
args = []                              # or ["--read-only"] for the cautious default
env_vars = ["DISPLAY", "XAUTHORITY"]   # forward X11 access from Codex's environment
startup_timeout_sec = 10               # optional (default 10)
tool_timeout_sec = 120                 # optional (default 60); must exceed any bounded watch

[mcp_servers.grab.env]
GRAB_BIN = "/absolute/path/to/grab"    # literal values
```

## Implementation checklist (when building the server)

1. Speak MCP over stdio (JSON-RPC): implement `initialize`, `tools/list`,
   `tools/call`. Reserve **stdout exclusively for JSON-RPC**; send all logs to
   stderr. Populate `tools/list` from `tools.json` (names, descriptions,
   `annotations`, schemas). Consider a short server `instructions` string so
   hosts get workflow guidance without a separate skill.
2. For each `tools/call`, use an explicit **per-tool argv adapter** (flag
   renames, mutual exclusion, value conversion) — never build argv from the `cli`
   string, and never invoke `grab` through a shell.
3. Wrap results in a valid `CallToolResult`: return `capture` output as MCP
   **image content** (not just a path), set `isError: true` on grab's non-zero
   exit (surfacing stderr), and add `structuredContent`/`outputSchema` where useful.
4. Validate inputs against the schemas (`oneOf`/required, reject unknown props).
5. Bound and make cancellable any long-running tool (`watch_capture`), cleaning
   up the child process on cancel.
6. Honor `GRAB_BIN` / an absolute path so the server finds the CLI, and forward
   the X11 environment (`DISPLAY`, `XAUTHORITY`) it needs.
7. Implement `--read-only` (register only `observe`+`capture`) and an allowlist
   for the mutating categories.
8. Keep it a single self-contained executable so it can ship under the plugin's
   `bin/` and be added to Codex/Claude in one line.
