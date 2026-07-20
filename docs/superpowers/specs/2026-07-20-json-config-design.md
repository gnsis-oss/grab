# grab JSON configuration — design

**Date:** 2026-07-20 (rev 2, after external design review)
**Status:** approved
**Deliverable:** typed config layer (`grab::config`, public header
`include/grab/config.hpp`) parsed with the in-tree nlohmann/json dependency,
config-driven `grab watch` (interval captures, mouse script, daemon mode,
stop/status) and `grab batch` (app-launch targets, manifest, comparison).

## Purpose

Port the legacy screengrab configuration layer (`screengrab.toml` +
`profiles/*.toml`, loaded by `screengrab.config` and consumed by the `watch`
and `batch` commands) into grab, as JSON. A profile file sets the rate of
continuous screenshots, the capture target, output naming, rotation limits,
batch app-launch targets, notifications, and comparison — and, new in grab,
a scripted sequence of cursor positions synthesized via `grab::Input` while
captures run.

## Decisions (from brainstorming + review)

- **Consumer:** the `grab` CLI (`watch`/`batch` verbs), backed by a library
  module — a shipped feature, not an example.
- **Format:** JSON (user decision; the legacy files were TOML).
- **Parsing:** the nlohmann/json v3.11.3 dependency already fetched and
  publicly linked by `grab_core` (`cmake/AddJson.cmake`) and used by session
  records, the locator, the browser bridge, storage, and doctor. Rev 1's
  in-tree RFC 8259 parser is dropped — review found the premise ("no JSON
  reader in-tree") false. One JSON stack; no corpus scope (§8.1 targets
  parsers we write, not the vendored canonical one).
- **"Scripting of positions":** cursor steps — move / click / click_at /
  drag / type / key / delay — executed through `grab::Input`, interleaved
  with captures at step granularity.
- **Scope:** full legacy parity, including watch daemon mode, `watch stop`
  / `watch status`, multi-config watch, and CLI overrides (user decision).

### Deliberate deviations from legacy

- TOML → JSON; `interval_s` → `interval_ms`; target key `class` →
  `wm_class`. Legacy profiles do not convert mechanically; the shipped
  `profiles/*.json` are the converted equivalents.
- Unknown keys are **hard errors** with their JSON Pointer (legacy ignored
  them). A required `"schema_version": 1` gates future evolution; an
  unsupported version is a distinct error, not an unknown-key error.
- `targets[].command` (one shell string, legacy `shell=True`) →
  `targets[].argv` (non-empty string array, direct exec, no shell) —
  removes shell-quoting ambiguity and injection surface.
- Config discovery: cwd-upward search for a config that can launch
  processes and synthesize input is an attack vector (hostile checkout).
  `--config PATH` is required; `$GRAB_CONFIG` serves as the fallback when
  the flag is omitted. No directory walking.
- Tray notifications and `coalesce_window_ms` dropped (tray is deferred
  scope); `strategy` accepts `"os"`/`"none"` only. Batch notifies per
  captured frame (legacy parity).
- Display backends `xvfb_vgl` and `weston` rejected with a "deferred"
  error; only `native` and `xvfb` ship.
- `defaults.window` dropped (no consumer in legacy either). Comparison
  settings live in one top-level `compare` section instead of legacy's
  split `[defaults.compare]` + `settings` keys.
- `update_config` (tomlkit format-preserving writes) dropped.
- Fixes over legacy, kept deliberately: omitted `compare.ref` disables
  comparison (legacy defaulted to `"."` and usually enabled it);
  target-level `timeout_s`/`kill_after` override the defaults section
  (legacy let global `settings.timeout_s` beat the target and ignored
  global `kill_after`); files missing from or extra in the compared
  directory are failures (legacy silently ignored them); script/step
  failures stop the script but not the captures.

## Parsing and validation — `src/config/`

`nlohmann::json::parse` with comments disallowed, wrapped to grab
conventions:

- Exceptions are caught at the parse boundary and converted to
  `grab::Result` errors carrying the byte offset and nlohmann's diagnostic.
- A parse callback enforces a nesting-depth cap (64) and rejects duplicate
  object keys (nlohmann's default keep-last is unacceptable for config).
- Decoding into the typed structs is hand-written (no ADL magic): every
  field checked for JSON type, range, and membership per the field table.
  Integer fields require JSON integers (`1.0`, `1e0` rejected). Errors
  carry the config path plus a JSON Pointer (`/watch/limits/max_files`),
  so keys containing dots and array indices are unambiguous.
- The whole file is validated regardless of verb — a typo in `targets`
  fails `grab watch` too. One config, one validity.
- Path values: a leading `~/` expands to `$HOME`; other `~` forms are
  errors. Relative paths resolve against the **config file's directory**,
  never the cwd. Rendered output filenames must be relative and must not
  contain `..`.

## Config model

Typed structs, one per section: `Config` — `DefaultsSection`,
`DisplaySection`, `WatchSection`, `ScriptSection`, `TargetSpec` (vector),
`NotifySection`, `CompareSection`. `load(path)` →
`grab::Result<Config>`; `resolve(explicit_path_or_empty)` applies the
explicit-path / `$GRAB_CONFIG` precedence (missing explicit file is an
error; absent `$GRAB_CONFIG` with no explicit path is a "no config"
error). Explicit paths are `--config PATH` for batch and the positional
`CONFIG...` args for `watch start`; `$GRAB_CONFIG` is the fallback only
when none are given (and yields a single config).

### Defaults — explicit field-by-field application (no generic deep merge)

| defaults field | applies to | direction |
|---|---|---|
| `format` | `watch.format`, target output format | fallback when unset |
| `timeout_s` | `targets[].timeout_s` | fallback; target wins |
| `kill_after` | `targets[].kill_after` | fallback; target wins |
| `output_root` | base for relative `watch.output` and `batch.output_root` | prefix |

### Normative field table

`schema_version` (integer, required, must equal 1).

**`defaults`** (optional): `format` string ∈ {`"png"`} default `"png"`;
`timeout_s` number > 0 default 15; `kill_after` bool default true;
`output_root` path default config dir.

**`display`** (optional; omitted = `native`): `backend` string ∈
{`"native"`, `"xvfb"`} default `"native"`; `width`,`height` integers
1–65535 default 1920×1080 (xvfb only); `depth` integer ∈ {8,16,24,32}
default 24 (xvfb only). Both watch and batch honor this section
identically: `xvfb` starts a `grab::screen::VirtualDisplay` whose DISPLAY
is used by capture, input synthesis, and spawned targets.

**`watch`** (required for `grab watch`): `interval_ms` integer ≥ 20;
`output` path (required); `filename` pattern string default
`"capture_{timestamp}"`; `format` string ∈ {`"png"`};
`target` object with **exactly one** of `title` (substring), `wm_class`,
`pid` (integer > 0), `window_id` (string `0x…`) — omitted section means
full display; `limits` object: `max_files` integer ≥ 1 (0/omitted =
unlimited), `max_age_days` integer ≥ 1 (0/omitted = unlimited),
`max_disk_mib` integer ≥ 1 (0/omitted = unlimited).

**`script`** (optional; watch only): `loop` bool default false; `steps`
non-empty array. Steps by `action`:
`move` {`x`,`y` int16}; `click` {`button` int 1–9 default 1};
`click_at` {`x`,`y` int16, `button` default 1}; `drag` {`from`,`to` =
[int16,int16]}; `type` {`text` string}; `key` {`name` string, xkb key
name}; `delay` {`ms` integer ≥ 1}. With `loop: true` the step list must
contain at least one `delay` totaling ≥ 100 ms per cycle (validated) —
prevents busy-spin.

**`targets`** (required for `grab batch`, non-empty array): `name` string,
unique, filesystem-safe (`[A-Za-z0-9._-]+`); `argv` non-empty string
array; `env` object of string→string, **overlaid** onto the inherited
environment (a `DISPLAY` key here is an error when `display.backend` is
`"xvfb"` — the virtual display owns DISPLAY); `match` string ∈ {`"pid"`,
`"wm_class"`, `"title"`} default `"pid"`; `pattern` string (required for
`wm_class`/`title`, forbidden for `pid`); `frames` integer ≥ 1 default 1;
`interval_ms` integer ≥ 20 default 200; `delay_ms` integer ≥ 0 default
1000; `timeout_s`, `kill_after` — see defaults.

**`batch`** (optional): `output_root` path default
`{defaults.output_root}/sessions`.

**`notifications`** (optional): `enabled` bool default false; `strategy`
string ∈ {`"os"`, `"none"`} default `"os"`; `popup_timeout_ms` integer
≥ 0 default 2000 — the desktop popup expiry passed in
`Notification.timeout_ms` (the D-Bus call timeout stays the
`NotifyOptions` internal default). Truth table: notifications fire iff
`enabled && strategy == "os"`; `enabled:false` with any strategy and
`enabled:true, strategy:"none"` are valid and silent.

**`compare`** (optional): `mode` string ∈ {`"exact"`, `"rmse"`} default
`"rmse"`; `threshold` number ≥ 0 default 5.0 (rmse only); `ref` path —
omitted disables comparison.

### Annotated example

The block below is annotated JSONC for reading; the shipped
`profiles/*.json` are strict JSON (comments are rejected by the parser).

```jsonc
{
  "schema_version": 1,
  "defaults": { "timeout_s": 15, "kill_after": true },
  "display": { "backend": "xvfb", "width": 1280, "height": 720 },
  "watch": {
    "interval_ms": 10000,
    "output": "~/.cache/grab/watch",
    "filename": "capture_{timestamp}",
    "target": { "wm_class": "gnome-terminal" },
    "limits": { "max_files": 500, "max_age_days": 7, "max_disk_mib": 2000 }
  },
  "script": {
    "loop": true,
    "steps": [
      { "action": "move", "x": 100, "y": 200 },
      { "action": "click", "button": 1 },
      { "action": "drag", "from": [100, 200], "to": [300, 400] },
      { "action": "type", "text": "hello" },
      { "action": "key", "name": "Return" },
      { "action": "delay", "ms": 500 }
    ]
  },
  "targets": [
    { "name": "editor", "argv": ["gedit", "--new-window"],
      "match": "wm_class", "pattern": "gedit",
      "frames": 5, "interval_ms": 200, "delay_ms": 1000 }
  ],
  "notifications": { "enabled": true, "strategy": "os" },
  "compare": { "mode": "rmse", "threshold": 5.0, "ref": "./ref" }
}
```

### Filename patterns

`{timestamp}` → UTC `YYYYMMDDTHHMMSS.mmm` (millisecond resolution —
`interval_ms` may be sub-second); `{date}` → `YYYYMMDD`; `{time}` →
`HHMMSS`; `{seq}` → zero-padded 5-digit counter starting at the count of
existing pattern-matching files in the output dir. An identical rendered
name overwrites (legacy behavior). The `.png` extension is appended if the
pattern doesn't end in it.

## `grab watch` — full parity surface

```
grab watch start CONFIG... [--daemon] [--interval MS] [--output DIR]
grab watch stop
grab watch status [--json]
```

The existing flag-driven title-change mode (`grab watch --window --out`)
stays untouched.

- **Multi-config:** one watcher thread per config file, each with its own
  `Screen`/`Input` connections and scheduler. `--interval`/`--output`
  override every config's corresponding value (legacy parity).
- **Daemon mode:** `--daemon` double-forks/`setsid`s, writes
  `$XDG_RUNTIME_DIR/grab/watch.pid`, redirects stdio to
  `$XDG_RUNTIME_DIR/grab/watch.log`, and maintains
  `$XDG_RUNTIME_DIR/grab/watch-status.json` (per-config: config path,
  captures, errors, paused flag, last capture time; rewritten atomically
  each tick). `stop` reads the pid file, sends SIGTERM, waits up to 10 s,
  reports. `status` reads the status file and verifies liveness via
  `/proc`; stale pid files are reported and removed. A second `start`
  while the pid file names a live process is an error. Foreground mode
  handles SIGINT/SIGTERM identically (existing signal plumbing).
- **Target resolution:** the configured match is resolved to a concrete
  X11 window id at start via enumeration; that exact window is captured
  every tick (capture-by-id — exposed from the existing internal route as
  an internal seam, no public `grab::Screen` API change). Zero matches:
  retry each tick, logged once. Multiple matches: first, logged. Window
  gone: re-resolve; failure counts as a capture error for that tick.
- **Cadence:** fixed-delay, legacy semantics — capture immediately on
  start, then arm the next deadline `interval_ms` after the tick's work
  completes. Missed deadlines (long step/capture) are skipped, never
  bursted, and counted in status.
- **Error policy:** per-tick capture/encode/write failures are logged and
  counted; the watcher continues. Notification failures are logged once
  and never affect capture outcome. Virtual-display or config errors at
  startup are fatal. Script step failure stops the script (recorded in
  status); captures continue.

### Rotation limits — ownership and semantics

Limits govern only files this profile's pattern matches in its own
`watch.output` directory (the ledger: startup scan of pattern-matching
files + every file this run writes). Unrelated files are never touched.
`max_files`: delete-oldest (by mtime) after each write. `max_age_days`:
prune ledger files older than the cap at startup and once per hour.
`max_disk_mib` (MiB): running total from the ledger; at the cap, pause
capturing, log and notify once; re-`stat` the ledger each minute while
paused and resume below 90 % of the cap (hysteresis, legacy parity).
Symlinks in the output dir are never followed or deleted.

### Script scheduler

One thread per config; a deadline loop over {next capture, next step}
waits on a timerfd (no raw sleeps — house rule; drag itself is sleepless
by design in `x11_drag_recipe`). Steps are atomic: each maps to one
synchronous `grab::Input` call, and yielding to a capture deadline happens
**only at step boundaries** — a long `type`/`drag` can overrun a deadline,
in which case the capture fires at the next boundary and skipped deadlines
are counted. Equal deadlines: capture before step. Ctrl-C during a delay
interrupts the wait and shuts down cleanly.

## `grab batch --config batch.json` — target runner

The current flag-driven simple batch stays. Config mode:

1. Start the virtual display if configured (fatal on failure).
2. Create `<batch.output_root>/<UTC YYYYMMDDTHHMMSS>_<profile-stem>/`
   (suffix `_2`, `_3`… on collision) with `current/` and a
   **`BatchManifest`** — a new JSON manifest (this feature's type, not
   `SessionRecord`, which is a workspace-lifecycle record with unrelated
   required fields): profile path, start/end times, state
   (`running`/`done`/`failed`), per-target name, argv, pid, window id,
   files, error, compare results. Written atomically (temp + rename)
   after every target and at exit; a crash leaves `state: "running"` as
   the tell.
3. Per target: materialize the full environment (inherited overlaid with
   `env`, DISPLAY forced to the virtual display when configured) and
   `OwnedProcess::spawn` the argv — spawn replaces the child environment
   wholesale, so the overlay is materialized before the call. Wait for
   the window by `match` within `timeout_s`: `pid` matches
   `_NET_WM_PID` against the spawned pid (documented caveat: launchers,
   daemonizing apps, and D-Bus-activated apps re-parent — use `wm_class`
   for those); `wm_class`/`title` match `pattern`. Settle `delay_ms`,
   capture `frames` shots `interval_ms` apart as `name.png` /
   `name_{i:03}.png` (notify per frame when enabled), then `kill_after`:
   SIGTERM, 2 s grace, SIGKILL, reaped via a new
   `OwnedProcess::wait(timeout)` (pidfd `waitid`; direct child only — no
   process-group kill, documented). Early child exit before a window
   appears is that target's error. Per-target errors are recorded; the
   run continues.
4. If `compare.ref` is set: directory comparison of `current/` vs `ref`
   (below); per-file results into the manifest; print pass/fail counts.
5. Exit 0 only if no target errors and no compare failures.

### Window enumeration fallback (prerequisite)

Enumeration currently requires the WM-maintained `_NET_CLIENT_LIST`; a
bare `VirtualDisplay` (Xvfb, no WM) yields no windows and every wait
would time out. Add a query-tree fallback: when `_NET_CLIENT_LIST` is
absent, walk `xcb_query_tree` from the root collecting mapped top-level
windows and read class/title/pid properties directly. Benefits watch,
batch, and any WM-less X server.

## Comparison — `grab::image` additions

`compare_files` (two files) and pixel-tolerance `compare` stay. New:

- `rmse` metric: per-pixel RMSE over 8-bit channels (0–255 scale),
  pass iff score ≤ `threshold`. `exact`: pass iff zero differing pixels.
- `compare_dirs(ref, current, mode, threshold)`: pairs files by name;
  a file missing from either side is a **failure** for that name
  (declared fix over legacy's silent skip). Returns per-file results.

## Testing

- Config decode: per-section tests against the field table (types,
  ranges, defaults, required/optional), defaults application, JSON
  Pointer error paths, unknown-key/version-gate/duplicate-key/depth
  rejection. Error assertions check structured fields (path, pointer,
  reason code), not prose.
- Scheduler: unit tests with an injected clock — cadence (fixed-delay),
  step/capture interleave, overrun-skip accounting, loop validation.
- Rotation: ledger ownership (foreign files untouched), delete-oldest,
  age prune, disk pause/resume hysteresis — tmpdir tests.
- Integration (Xvfb pattern from the example smokes: `-noreset`,
  `_NET_WM_CM_S0` stub — plus the new query-tree fallback removes the
  `_NET_CLIENT_LIST` stub requirement): config watch + script e2e
  (file count, pattern, rotation); batch e2e spawning a test X client
  (frames, manifest states, compare outcomes); daemon start/status/stop
  e2e.
- Shipped profiles: `profiles/watch-10s-notify.json`,
  `profiles/watch-1m-notify.json` (converted from the legacy TOML
  profiles) and `profiles/batch-example.json` (authored fresh — legacy
  shipped no batch profile).
