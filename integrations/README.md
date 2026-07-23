# Using grab with AI agents

grab is built to be driven by AI coding agents. This directory holds the files
that let Claude and Codex **discover** grab and **call** its features. Three
surfaces, layered:

| Surface | Gives the agent | Claude | Codex | Location |
| --- | --- | --- | --- | --- |
| **Skill** | Knows when/how to use grab | ✅ | — | `integrations/claude/skills/grab/SKILL.md` |
| **Plugin + marketplace** | One-command install of the skill (and MCP) | ✅ | — | `integrations/claude/`, `.claude-plugin/marketplace.json` |
| **MCP server** (`grab-mcp`) | Typed, callable tools | ✅ | ✅ | `mcp/` (spec; server not built yet) |
| **AGENTS.md snippet + CLI `--json`** | Context for any shell-driven agent | ✅ | ✅ | below |

The skill and MCP are complementary: the **skill** teaches judgment (when to
reach for grab, safe workflow), the **MCP server** provides actuation (callable
tools with schemas). Ship both for the best experience.

## Claude Code

Install everything from the grab repo, which is itself a plugin marketplace:

```
/plugin marketplace add gnsis-oss/grab
/plugin install grab@grab
```

That loads the `grab` skill immediately. To also get callable tools, build
`grab-mcp` (see [`../mcp/README.md`](../mcp/README.md)) and add its `mcpServers`
block — either to the plugin or to your project `.mcp.json`.

## Codex

Codex has no skills; it reads `AGENTS.md` and speaks MCP. Two steps:

1. Add the MCP server to `~/.codex/config.toml`:
   ```toml
   [mcp_servers.grab]
   command = "grab-mcp"
   args = []
   ```
2. Paste the snippet below into your project's `AGENTS.md` so Codex knows grab
   exists and how it is shaped.

## AGENTS.md snippet (copy into your project)

```markdown
## grab — desktop automation (Linux/X11)

`grab` is a CLI for OS-level desktop interaction: capture window screenshots,
enumerate/focus/place windows, and synthesize mouse/keyboard input.

- Check the environment first: `grab doctor --json`.
- Discover windows: `grab windows --json` (gives id, WM_CLASS, title, bounds).
- Capture (read-only): `grab capture --window-id <id> --out shot.png`.
- Control/synthesize (mutating): `grab focus`, `grab place`, `grab click`,
  `grab type --text`, `grab key`, `grab drag`.
- Prefer `--json`; run `grab <verb> --help` for flags. Input synthesis acts on
  the real desktop — capture and inspect before any mutating verb.
```

## Any agent that shells out

grab needs nothing special to be usable from a plain shell: `grab --help` lists
verbs, most read verbs support `--json`, and each verb has `grab <verb> --help`.
The skill/MCP files above just make discovery automatic instead of manual.

## Note on file locations

`AGENTS.md`, `CLAUDE.md`, and `.claude/` are gitignored in this repo (local
assistant residuals), so the shippable, tracked integration lives here under
`integrations/` and `mcp/`, plus the root `.claude-plugin/marketplace.json` that
makes the repo installable. There is deliberately no tracked root `AGENTS.md` —
use the snippet above in *your* project instead.
