# Event logger example — design

**Date:** 2026-07-19
**Status:** approved
**Deliverable:** `examples/event_logger.cpp` — a live terminal feed of every
event grab observes, plus a faithful JSONL recording.

## Purpose

A runnable demonstration of grab's observation surface: start it, use your
desktop, and watch the system narrate itself one line per event:

```
14:02:13.114 -> browser event -> tab "GitHub - grab" focused
14:02:11.481 -> os event      -> application "firefox" opened window "Home"
```

It complements `mouse_snake_trail` (synthesis + overlay) by showcasing the
other half of the library: subscription, the event vocabulary, and storage.

## Decisions (from brainstorming)

- **Scope:** all event kinds — semantic, discrete input, and continuous input,
  with continuous streams coalesced for the terminal.
- **Recording:** terminal feed plus a JSONL file via `grab::storage::JsonlSink`.
- **Lifecycle:** runs until Ctrl+C (SIGINT/SIGTERM), then clean shutdown.
- **Form:** self-contained in-process example (own `grab::Session`), not a
  daemon client and not a CLI verb.

## Architecture

One new file `examples/event_logger.cpp` plus an `examples/CMakeLists.txt`
entry (links the snake-trail set minus screen/image/codec, plus the storage
target). Components, all in an anonymous namespace:

1. **`EventFormatter`** — pure: `grab::Event` → one text line. Kind→category
   and fallback naming come from the central `EventDescriptor` table
   (`include/grab/event_descriptor.hpp`) — the single-source vocabulary; no
   name tables are re-scattered in the example. Descriptions come from the
   payload variant.
2. **`MoveCoalescer`** — accumulates continuous `MouseMove` samples per
   axis-group (pointer position vs scroll) and emits one summary line per
   ~300 ms window: last position / summed delta plus sample count. All other
   kinds pass through untouched (the descriptor table's `CoalescingClass`
   marks discrete kinds `NeverDrop`).
3. **`LogPump`** — the proven snake-trail notify→post→drain pattern:
   subscription notify posts a drain job on the session reactor; drain pops
   `SubscriptionEvent`s into the consumer. Reactor-thread-only; same
   shutdown fence (`stop()` = unset notify → `stop_observation()` → posted
   fence promise).
4. **`EventLogger` (consumer)** — per event: coalescer → formatter → stdout,
   and the raw event → `JsonlSink::write()`. Sink directory `./event-log/`
   by default, overridable as `argv[1]`.

### Data flow

observers (XInput2 input, window trackers, browser bridge, AT-SPI2) →
EventBus → subscription queue (empty `SubscriptionScope` = all kinds) →
notify → reactor drain → coalescer/formatter → stdout + JSONL.

The main thread blocks SIGINT/SIGTERM and sits in the CLI's existing
`sigtimedwait` poll pattern; each ~300 ms tick posts a coalescer-flush job to
the reactor, so a pointer that stops moving still gets its final summary line
without new timer machinery. Browser lines appear only while the webextension
bridge is connected; absence is not an error — the feed simply carries
os/input events.

## Output format

`timestamp -> <category> event -> <description>`, category column padded so
descriptions align.

- **Timestamp:** `event.timestamp` (epoch seconds, double) as local wall
  clock `HH:MM:SS.mmm`.
- **Category labels** via `EventCategory`: `Input` → `input`, `Window` → `os`,
  `Integration` → `browser`, `Accessibility` → `a11y`, `State` and graph
  kinds → `state`.

Representative lines:

```
14:02:11.481 -> os event      -> application "firefox" opened window "Home - Mozilla Firefox"
14:02:11.502 -> os event      -> window "Home - Mozilla Firefox" (firefox) focused
14:02:13.114 -> browser event -> tab "GitHub - grab" focused
14:02:14.300 -> browser event -> context updated: "https://github.com/..."
14:02:15.020 -> input event   -> key "a" pressed
14:02:15.180 -> input event   -> combo "ctrl+c" pressed
14:02:16.444 -> input event   -> button "left" clicked
14:02:17.001 -> input event   -> pointer moved to (812, 440) [23 samples]
14:02:19.310 -> input event   -> scrolled vertical by -6 [4 samples]
14:02:40.000 -> input event   -> idle started (30.0s)
14:02:52.700 -> a11y event    -> button "Save" clicked in "gedit"
14:03:01.220 -> os event      -> window title changed "old" to "new" (firefox, 48.2s)
14:03:05.850 -> os event      -> application "gedit" closed window "untitled"
```

- `QueueGapMarker` → visible `!! gap: events dropped after seq N` line.
- State/graph kinds → terse one-liners (`state event -> snapshot (1.2 KB)`,
  `state event -> node added #42`).
- Unknown/future kinds → fall back to the descriptor wire name; nothing is
  silently dropped.
- **JSONL is unthrottled:** every raw event, including each individual
  mouse-move sample, is written to the sink. The recording stays faithful;
  only the terminal is coalesced.

## Shutdown

Ctrl+C → poll loop notices → post final coalescer flush → `pump.stop()`
(reactor fence — no drain job outlives the consumer) → `session->close()` →
`JsonlSink::flush()`/`close()` → summary line
(`N events observed, M lines printed, recording: <dir>/<date>.jsonl`) →
exit 0.

## Error handling

- Startup failures (display, session, sink open) → exit 1 with the
  `grab::Error` message, same plumbing style as `mouse_snake_trail::run()`.
- Runtime sink write failure must not kill the live feed: first error prints
  one stderr warning, recording stops (no retry spam), the feed continues,
  and the process exits 1 with that error in the summary.
- Stdout write failures are best-effort ignored.

## Testing / verification

- No GTest suite for examples (matches `mouse_snake_trail`); the build gate
  is `GRAB_BUILD_EXAMPLES` compiling clean under the default ASan/UBSan +
  clang-tidy preset.
- Scripted smoke: run under Xvfb (no compositor stub needed — no overlay),
  drive `grab::Input` synthesis from a second process, then assert
  (a) expected stdout lines including one coalesced pointer line, and
  (b) the JSONL file exists and holds more move events than lines printed.
- Browser-event lines verified manually on a real display with the
  webextension attached (the bridge needs a live browser).

## Out of scope

- No daemon/gRPC client path, no CLI verb, no Wayland, no color/TTY theming,
  no filtering flags — the example stays a minimal readable demonstration.
