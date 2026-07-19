# Event logger example — design

**Date:** 2026-07-19 (revised same day after external review)
**Status:** approved; revised for review findings, pending re-approval
**Deliverable:** `examples/event_logger.cpp` — a live terminal feed of every
event grab observes, plus a best-effort JSONL recording — and one small
library pre-fix (uniform event timestamps).

## Purpose

A runnable demonstration of grab's observation surface: start it, use your
desktop, and watch the system narrate itself one line per event:

```
14:02:13.114 -> browser event -> tab "GitHub - grab" focused
14:02:11.481 -> os event      -> application "firefox" opened window "Home"
```

It complements `mouse_snake_trail` (synthesis + overlay) by showcasing the
other half of the library: subscription, the event vocabulary, composition,
and storage.

## Decisions (from brainstorming + review)

- **Scope:** all event kinds — semantic, discrete input, and continuous input,
  with continuous streams coalesced for the terminal.
- **Recording:** terminal feed plus JSONL via `grab::storage::JsonlSink`;
  recording is **best-effort** (bounded queues), with drops surfaced as
  visible gap lines and a summary count — never silently.
- **Lifecycle:** runs until Ctrl+C (SIGINT/SIGTERM), then clean shutdown with
  an explicit tail drain.
- **Form:** self-contained in-process example (own `grab::Session` plus a
  local producer stack, mirroring the CLI's own composition pattern).
- **Timestamps:** fixed at the library, not papered over in the example
  (see "Library pre-fix").
- **Browser events:** the example hosts the webextension bridge behind a
  unix socket so real browser tabs can appear (see "Browser feed").

## Library pre-fix: uniform event timestamps (prerequisite)

Review finding (verified): `event.timestamp` mixes clocks. X11 input events
stamp `raw.time` — an X-server *milliseconds* counter
(`src/drivers/desktop/x11/x11_event_source.cpp`) — while the window tracker
and AT-SPI stamp `system_clock` epoch seconds, and graph/browser events can
carry zero. This breaks any wall-clock rendering **and** misroutes
`JsonlSink`'s daily files library-wide (the sink partitions by timestamp
interpreted as epoch seconds), affecting the daemon's recordings too.

Pre-fix, driver/kernel-local, shipped before the example:

1. `x11_event_source` stamps `system_clock` epoch seconds (like the window
   tracker already does) instead of `raw.time`.
2. Graph-event translation (`session_impl`) stamps epoch seconds instead of
   leaving zero.
3. The browser bridge stamps arrival epoch seconds when a frame carries no
   (or zero) timestamp.

Existing tests asserting `raw.time` propagation are updated. After this,
"epoch seconds, double" is a real invariant and the example renders local
`HH:MM:SS.mmm` directly.

## Architecture

One new file `examples/event_logger.cpp` plus an `examples/CMakeLists.txt`
entry (links `grab_core`, `grab_kernel`, `grab_session`, `grab_driver_x11`
(for `WindowTracker`), `grab_driver_webextension` (for `BrowserBridge`), and
`grab_storage`). Components, all in an anonymous
namespace:

1. **`EventFormatter`** — pure: `grab::Event` → one text line. Kind → category
   and fallback naming come from the central `EventDescriptor` table; a small
   **display-label map** turns categories into feed labels (`Input` → `input`,
   `Window` → `os`, `Accessibility` → `a11y`, `Integration` → `browser`,
   `State` → `state`); graph kinds (the 700-range) display as `ui`
   (their descriptor category is `Window`; the label map special-cases them).
   Descriptions come from the payload variant; when a producer leaves a
   name empty (X11 fills only key/button *codes*), the formatter falls back
   to numeric (`key #38 pressed`).
2. **`MoveCoalescer`** — accumulates continuous `MouseMove` samples and emits
   one summary line per ~300 ms window: last position (or summed axis delta)
   plus sample count. All other kinds pass through untouched. The example
   decides this itself; it does **not** claim the descriptor table's
   `CoalescingClass` marks every discrete kind `NeverDrop` (it doesn't).
3. **Producer stack** — mirrors what the CLI composes for itself:
   - `grab::Session` (X11 input runtime, best-effort AT-SPI, graph/tree
     events) with `watch( {}, QueueOptions{ .capacity = 8192,
     .overflow = NeverDrop } )` — empty scope = all descriptor-registered
     kinds (verified; note this expands at subscribe time, it is not a
     persistent wildcard).
   - `WindowTracker` and any `BrowserBridge` instances (below) are started
     **on the session's own reactor and bus** via the public
     `Session::reactor()` / `Session::bus()` composition seam (the owning
     Session does not compose a tracker itself — the CLI starts its own the
     same way, `src/frontends/cli/main.cpp:1740`, just against a private
     bus). One bus, one subscription, one pump — no second reactor thread.
4. **`LogPump`** — one drain job on the **session reactor**, triggered by
   the subscription's notify (posts via `session->post`, guarded by one
   `scheduled_` flag). All consumption is single-threaded on the session
   reactor. Same fence discipline as `mouse_snake_trail`, plus the explicit
   tail drain at shutdown (below).
5. **`EventLogger` (consumer)** — per event: coalescer → formatter → stdout,
   and the raw event → `JsonlSink::write()`. Sink lives on the drain path
   (single-threaded use — the sink is not thread-safe); its periodic
   fsync-every-`buffer_limit` writes are an accepted, bounded stall for an
   example. Sink directory `./event-log/` by default, `argv[1]` overrides.
   The sink partitions by UTC event date (`<YYYY-MM-DD>.jsonl`), appends
   across runs, and retains 30 files / 500 MiB — documented, not fought.

### Browser feed

`BrowserBridge` requires a native-messaging FD and nothing in-tree composes
one. The example makes browser lines real:

- It listens on a unix socket (default
  `$XDG_RUNTIME_DIR/grab-event-logger.sock`, flag-overridable) registered on
  the session reactor via `add_fd`; each accepted connection is handed to a
  `BrowserBridge::start( fd, session->reactor(), session->bus() )`. The
  bridge does not take fd ownership — the example closes accepted fds after
  `stop()`.
- A documented native-messaging-host manifest plus a one-line forwarder
  (e.g. `socat STDIO UNIX-CONNECT:<sock>`) connects a real browser's
  extension to it. Without a connection the feed simply carries no browser
  lines — never an error.
- The bridge populates `IntegrationEvent::title` from the frame's `title`
  key; for frames that carry `tab_title` instead (as the bridge tests do),
  the formatter falls back to reading it from the raw `json` payload.

### Data flow

all producers (XInput2, AT-SPI, tree/graph via the composed session; plus
the example-started WindowTracker and BrowserBridge) → the **session bus** →
one subscription → notify → posted drain on session reactor →
coalescer/formatter → stdout + JSONL.

The main thread **blocks SIGINT/SIGTERM before `Session::open()`** (every
later-spawned thread — session reactor included — inherits the mask; blocking
after open would race a process-directed signal into an unblocked worker),
then sits in the CLI's `sigtimedwait` poll pattern; each ~300 ms tick posts a
coalescer-flush job so a pointer that stops moving still gets its final
summary line.

## Output format

`timestamp -> <category> event -> <description>`, category column padded so
descriptions align.

- **Timestamp:** `event.timestamp` (epoch seconds after the pre-fix) as
  local wall clock `HH:MM:SS.mmm`.

Representative lines — every line below is producible by a current producer
(review pruned the aspirational ones):

```
14:02:11.481 -> os event      -> application "firefox" opened window "Home - Mozilla Firefox"
14:02:11.502 -> os event      -> window "Home - Mozilla Firefox" (firefox) focused
14:02:13.114 -> browser event -> tab "GitHub - grab" focused
14:02:14.300 -> browser event -> context updated: "https://github.com/..."
14:02:15.020 -> input event   -> key "a" pressed        (falls back to: key #38 pressed)
14:02:16.444 -> input event   -> button "left" clicked  (falls back to: button #1 clicked)
14:02:17.001 -> input event   -> pointer moved to (812, 440) [23 samples]
14:03:01.220 -> os event      -> window title changed "old" to "new" (firefox)
14:03:05.850 -> os event      -> application "gedit" closed window "untitled" (48.2s)
14:02:52.700 -> a11y event    -> button "Save" clicked in "gedit"
14:02:53.000 -> ui event      -> node added #42
14:02:54.000 -> state event   -> snapshot (1.2 KB)
```

The formatter still covers the **entire** vocabulary (KeyCombo, Idle, every
graph kind, …) — kinds without a current producer simply print if they ever
arrive; unknown kinds fall back to the descriptor wire name. Duration is
shown only where producers fill it (window close), not on title changes.

- `QueueGapMarker` → visible `!! gap: events dropped after seq N` line and a
  counter in the exit summary. Gap markers cannot be written to JSONL
  (`JsonlSink::write()` accepts only `Event`) — the terminal and summary are
  their record.
- **Recording is best-effort:** large `NeverDrop` queues make drops unlikely,
  but under sustained overflow events are lost with a gap marker; the JSONL
  holds exactly the events that survived the queue. Mouse moves are recorded
  per-sample (the terminal alone is coalesced).

## Shutdown

Ctrl+C → poll loop notices → stop example-started producers (listener,
bridges, tracker) → unset notify → `stop_observation()` → **post one final
job that drains the queue to exhaustion and flushes the coalescer, then
fulfils the fence promise** (the snake-trail fence alone does not drain the
tail — verified; without this the last events before Ctrl+C are lost) →
`session->close()` → `JsonlSink::flush()` (checked — `close()` is void and
swallows errors) → `close()` → summary
(`N events observed, M lines printed, G gaps, recording: <dir>`) → exit 0.

## Error handling

- Startup failures (display, session, tracker, sink, socket) → exit 1 with
  the `grab::Error` message. Known limitation, documented in the example
  header: `Session` currently swallows the underlying X11 open error into a
  generic "no composed display stack" message, and a mid-run observer death
  is silent (observation-pump errors are discarded) — the example reports
  what the API surfaces.
- Runtime sink write failure must not kill the live feed: first error prints
  one stderr warning, recording stops, the feed continues, exit 1 with that
  error in the summary.
- Stdout write failures are best-effort ignored.

## Testing / verification

- Library pre-fix: unit tests updated/added for epoch-second stamping at all
  three producer sites.
- Example build gate: `GRAB_BUILD_EXAMPLES` compiling clean under the default
  ASan/UBSan + clang-tidy preset.
- Scripted smoke under Xvfb (no compositor stub needed):
  1. input lines — inject key/click/motion with `xdotool` (guarded: the
     input section is skipped when xdotool is absent); assert key/click
     lines and one coalesced pointer line;
  2. os lines — spawn/kill a window (e.g. `xterm`); assert
     created/focused/closed lines;
  3. browser lines — pipe a canned native-messaging frame into the unix
     socket with `socat`; assert a `browser event` line (no real browser
     needed);
  4. JSONL — file exists, holds more move events than pointer lines printed,
     timestamps are plausible epoch seconds.
- Real-browser wiring verified manually once (manifest + forwarder doc).

## Out of scope

- No daemon/gRPC client path, no CLI verb, no Wayland, no color/TTY theming,
  no filtering flags.
- No fix for Session's swallowed X11 open error or silent observer death
  (documented limitation; candidate follow-up).
- No KeyCombo/Idle/scroll producers (formatter-ready; producers are future
  library work).
