# grab JSON configuration — design

**Date:** 2026-07-20
**Status:** approved
**Deliverable:** in-tree JSON parser (`src/codec/json`), typed config layer
(`grab::config`, public header `include/grab/config.hpp`), and config-driven
`grab watch --config` / `grab batch --config` with a new mouse-script section.

## Purpose

Port the legacy screengrab configuration layer (`screengrab.toml` +
`profiles/*.toml`, loaded by `screengrab.config` and consumed by the `watch`
and `batch` commands) into grab, as JSON. A profile file sets the rate of
continuous screenshots, the capture target, output naming, rotation limits,
batch app-launch targets, notifications, and comparison — and, new in grab,
a scripted sequence of cursor positions synthesized via `grab::Input` while
captures run.

## Decisions (from brainstorming)

- **Consumer:** the `grab` CLI (`watch`/`batch` verbs), backed by a library
  module — a shipped feature, not an example.
- **Format:** JSON (user decision; the legacy files were TOML).
- **"Scripting of positions":** cursor steps — move / click / drag / type /
  key / delay — executed through `grab::Input`, interleaved with captures.
- **Scope:** full legacy parity — `defaults`, `display`, `watch`, `targets`,
  `notifications`, `compare` sections — plus the additive `script` section.
- **Deviations from legacy (deliberate):**
  - TOML → JSON.
  - Tray notifications and `coalesce_window_ms` dropped — the tray is
    deferred scope in the screengrab port; `strategy` accepts `"os"`/`"none"`
    only and errors on tray values, naming the deferral.
  - `update_config` (tomlkit format-preserving single-key writes) dropped —
    nothing in grab needs programmatic config edits.
  - Unknown keys are **hard errors** with their JSON path (legacy silently
    ignored them); typo protection for hand-authored files.
  - `defaults.window` dropped — no command consumed it in legacy either
    (dead config). Comparison settings live in one top-level `compare`
    section instead of legacy's split `[defaults.compare]` + `settings`
    keys.

## JSON parser — `src/codec/json.{hpp,cpp}`

No JSON reader exists in-tree (the webextension bridge parses only flat
frames; storage/session emit JSON as writers), and the dependency policy
forbids an external one.

- Strict RFC 8259 recursive-descent DOM parser. `json::Value`: null, bool,
  number (double, with exact `int64`/`uint64` preserved when representable),
  string, array, object. Arrays/objects use contiguous storage (flat vectors,
  no node-based maps).
- Nesting depth cap (constant, 64). No comments, no trailing commas, UTF-8
  validated. Duplicate object keys are an error.
- Parse errors carry the byte offset and expectation
  (`"offset 123: expected ':'"`), returned via `grab::Result`.
- **§8.1 corpus:** vendor JSONTestSuite under `tests/json_corpus/`; glob-based
  CTest harness — `y_*` must parse, `n_*` must reject cleanly, `i_*` either
  way, and no input may crash or trip ASan/UBSan. Side-by-side agreement
  script against `jq` as the canonical tool; investigate every disagreement.

## Config model — `src/config/`, `grab::config`

Typed structs, one per section, with a loader and validation:

- `Config` — top-level; `DefaultsSection`, `DisplaySection`, `WatchSection`,
  `ScriptSection`, `TargetSpec` (vector), `NotifySection`, `CompareSection`.
- `load(path)` → `grab::Result<Config>`: parse, validate types/ranges,
  reject unknown keys with JSON-path context
  (`"watch.limits.max_files: expected unsigned integer"`).
- `discover(explicit_path)` — legacy precedence preserved: explicit path if
  given (missing file is an error); else upward search from cwd for
  `grab.json`, stopping at a `.git` boundary or `$HOME`; else `$GRAB_CONFIG`.
- Deep-merge of `defaults` into the specific sections (legacy
  `merge_defaults` semantics: dicts merge recursively, scalars override).

### Schema reference

```jsonc
{
  "defaults": {
    "format": "png",             // only "png" in v1
    "timeout_s": 15,
    "kill_after": true
  },
  "display": {                   // omit section = use $DISPLAY
    "virtual": true,             // Xvfb via grab::screen::VirtualDisplay
    "size": [1280, 720],
    "depth": 24
  },
  "watch": {
    "interval_ms": 10000,        // rate of continuous screenshots
    "output": "~/.cache/grab/watch",
    "filename": "capture_{timestamp}",   // {timestamp}, {seq}
    "target": {                  // omit = full display; at most one key
      "title": "Firefox",        // substring match
      "wm_class": "gnome-terminal",
      "pid": 12345,
      "window_id": "0x1400003"
    },
    "limits": {
      "max_files": 500,          // delete-oldest after each capture
      "max_age_days": 7,         // prune on start and periodically
      "max_disk_mb": 2000        // pause capture (notify once) at cap
    }
  },
  "script": {                    // NEW — no legacy equivalent
    "loop": true,
    "steps": [
      { "action": "move",  "x": 100, "y": 200 },
      { "action": "click", "button": 1 },
      { "action": "click_at", "x": 400, "y": 300, "button": 1 },
      { "action": "drag",  "from": [100, 200], "to": [300, 400] },
      { "action": "type",  "text": "hello" },
      { "action": "key",   "name": "Return" },
      { "action": "delay", "ms": 500 }
    ]
  },
  "targets": [                   // batch mode
    {
      "name": "app",
      "command": "gedit --new-window",
      "env": { "GTK_THEME": "Adwaita" },
      "match": "pid",            // "pid" | "wm_class" | "title"
      "pattern": "",             // for wm_class/title matching
      "frames": 5,
      "interval_ms": 200,
      "delay_ms": 1000,          // settle before first frame
      "timeout_s": 15,
      "kill_after": true
    }
  ],
  "notifications": {
    "enabled": true,
    "strategy": "os",            // "os" | "none"
    "timeout_ms": 2000
  },
  "compare": {
    "mode": "rmse",
    "threshold": 5.0,
    "ref": "./screenshots/ref"   // omit = no comparison
  }
}
```

## `grab watch --config profile.json` — interval mode

The existing flag-driven title-change mode is untouched; passing `--config`
selects interval mode.

- Every `interval_ms`: capture the target window (located by the configured
  match) or the full display; write via the filename pattern; enforce limits;
  optionally send a D-Bus notification.
- Rotation: `max_files` delete-oldest after each write; `max_age_days`
  pruned at start and periodically; `max_disk_mb` tracked as a running byte
  total — at the cap, pause capturing, log and notify once, resume when the
  total drops below the cap.
- Ctrl-C stops and prints the capture summary (existing signal handling).

### Script scheduler

Single-threaded interleaving — no thread races between synthesis and capture:
one loop tracks the next capture deadline and the next script step. A step's
`delay` (and drag durations) yield to pending capture deadlines: the loop
always services whichever deadline comes first. With `loop: true` the step
list restarts; with `loop: false` captures continue alone after the last
step. Script execution uses `grab::Input::open` against the same display
(virtual or real) as capture. A failing step aborts the run with its error.

## `grab batch --config batch.json` — target runner

The current flag-driven simple batch (wm_class → out path) stays. Config mode
follows legacy `run_batch`:

1. If `display.virtual`, start `VirtualDisplay` — spawn, capture, and input
   all use its `DISPLAY`.
2. Create the session dir `<output>/<UTC yyyy-mm-ddThhmm>_<profile-stem>/`
   with a `current/` subdir and a session-record manifest
   (`src/session/record` JSON).
3. Per target: spawn `command` (`grab::OwnedProcess::spawn`) with merged env;
   wait for its window by `match`/`pattern` within `timeout_s` (pid matching
   uses the spawned pid via the window locator); settle `delay_ms`; capture
   `frames` shots `interval_ms` apart as `name.png` / `name_{i:03}.png`;
   `kill_after` terminates the process. Per-target errors are recorded, the
   run continues.
4. If `compare.ref` is a directory: compare `current/` against it with
   `mode`/`threshold` (existing `grab::screen`/`grab::image` compare),
   print pass/fail counts.
5. Exit 0 only if no target errors and no compare failures.

## Notifications

`grab::notify::Notifier` (D-Bus) when `enabled` and strategy `"os"`: one
notification per watch capture and per batch target completion, with
`timeout_ms`. No tray, no coalescing (deferred with the tray port).

## Testing

- Parser: unit tests (tokens, nesting, numbers, escapes, depth cap,
  duplicate keys, error offsets) + JSONTestSuite corpus + `jq` agreement
  (§8.1).
- Config: per-section decode tests, defaults merge, discovery precedence,
  unknown-key and type-error paths with exact message assertions.
- Integration (Xvfb pattern from the example smokes: `-noreset`,
  `_NET_WM_CM_S0` stub): config-driven watch + script capturing N frames
  while the cursor moves, asserting file count, filename pattern, and
  `max_files` rotation; batch e2e spawning a test window and asserting
  frames + manifest + compare outcome.
- Shipped profiles under `profiles/`: `watch-10s-notify.json`,
  `watch-1m-notify.json`, `batch.json` — direct ports of the legacy TOML
  profiles.
