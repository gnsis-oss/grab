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
agent *call* grab directly, with each tool advertising a name, a description, and
a JSON input schema. Those three fields are exactly what the model reads to pick
and populate a tool — so the tool catalog **is** the feature discovery surface.

## Transport & contract

- **Transport:** stdio. grab drives a live X11 display, so the server must run
  locally on the machine with the desktop session (or a virtual `Xvfb`). A
  hosted HTTP/SSE server cannot reach the user's screen and is not appropriate.
- **Implementation:** a thin adapter. Each tool call shells out to the matching
  `grab <verb>` with `--json` where available, and returns grab's JSON as the
  tool result. No desktop logic lives in the server — grab owns all of it.
- **Errors:** surface grab's non-zero exit and stderr as an MCP tool error; do
  not fabricate success.

## Safety model — read-only vs input-synthesis

Every tool in `tools.json` carries a `safety` field derived from grab's own
command mutability table:

- `read-only` — `doctor`, `list_windows`, `capture_screen`, `compare_images`,
  `watch_events`, `batch`. Observe/capture only; never move the pointer or type.
- `input-synthesis` — `focus_window`, `place_window`, `click`, `type_text`,
  `key`, `drag`. These act on the real desktop.

A server SHOULD support a `--read-only` mode that registers only the read-only
tools, and MAY gate the input-synthesis group behind an explicit allowlist. This
lets cautious users adopt capture/inspection without granting full desktop control.

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

```toml
[mcp_servers.grab]
command = "grab-mcp"
args = []
```

## Implementation checklist (when building the server)

1. Speak MCP over stdio (JSON-RPC): implement `initialize`, `tools/list`,
   `tools/call`. Populate `tools/list` straight from `tools.json`.
2. For each `tools/call`, translate arguments to `grab <verb>` flags per the
   `cli` field, run it, and return stdout (parsed JSON where grab emits it).
3. Honor `GRAB_BIN` / an absolute path so the server finds the CLI.
4. Implement `--read-only` and an optional allowlist for the input-synthesis set.
5. Keep it a single self-contained executable so it can ship under the plugin's
   `bin/` and be added to Codex/Claude in one line.
