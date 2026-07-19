# browser_event_screenshot example — design

**Date:** 2026-07-19
**Status:** approved
**Deliverable:** `examples/browser_event_screenshot.cpp` — capture a full-display
screenshot on every browser event and save it as a timestamped PNG — plus a
shared `examples/browser_socket.hpp` factored out of `event_logger`.

## Purpose

Demonstrate composing grab's observation and capture halves: a browser event
(observed via the webextension bridge) drives a `grab::Screen` capture. Run it,
browse, and each tab switch / context update leaves a timestamped PNG in a
folder.

## Decisions (from brainstorming)

- **Capture target:** the full display (`grab::Screen::display()`).
- **Trigger set:** browser events only — `EventCategory::Integration`
  (`AppTabChanged`, `AppContextUpdate`, and any future Integration kind).
- **Browser connection:** the same unix-socket + `BrowserBridge` mechanism as
  `event_logger`, extracted into a shared header (DRY).
- **Lifecycle:** runs until Ctrl+C.

## Shared extraction: `examples/browser_socket.hpp`

`event_logger.cpp` currently inlines a `BrowserSocket` (unix listener that
hands each accepted connection to a `BrowserBridge` on the session
reactor/bus) plus a `default_socket_path()` helper. Move both, verbatim, into
a new header-only `examples/browser_socket.hpp` in an
`grab::examples` namespace, and include it from both examples. The socket's
default path becomes a caller-supplied argument (each example passes its own
default: `grab-event-logger.sock` vs `grab-browser-shots.sock`) so the two
don't collide.

`event_logger.cpp` change: delete the inlined `BrowserSocket` +
`default_socket_path`, `#include "browser_socket.hpp"`, and pass its own
default socket name. No behavior change — verified by its existing smoke.

## Architecture

New file `examples/browser_event_screenshot.cpp` plus the shared header and an
`examples/CMakeLists.txt` entry (links `grab_core`, `grab_kernel`,
`grab_session`, `grab_platform_x11`, `grab_driver_x11`,
`grab_driver_webextension`, `grab_screen`, `grab_image`, `grab_codec`).
Components, anonymous namespace unless noted:

1. **`capture_filename( dir, epoch_s, kind_label ) -> std::filesystem::path`**
   — pure: `<dir>/<YYYYMMDD-HHMMSS-mmm>-<kind_label>.png` in local time.
   Unit-testable.
2. **`ScreenshotSaver`** — owns a `grab::Screen` and the output directory.
   `capture_and_save( const grab::Event& ) -> grab::Result<std::filesystem::path>`
   calls `Screen::display()` → `grab::codec::encode_png()` → writes the file
   (binary `ofstream`). Runs on the session reactor thread (the drain).
   Browser events are infrequent, so the synchronous capture stall is
   acceptable. First failure warns once on stderr and is remembered; the
   process keeps observing and exits 1 with that error in the summary.
3. **`BrowserSocket` (shared header)** — listener → `BrowserBridge` per
   connection, on the session reactor/bus.
4. **`CaptureLogger` (consumer)** — per browser event: `capture_and_save`,
   then print `saved <path> (<description>)` where description is the tab
   title / context (reusing the same `IntegrationEvent` fields event_logger
   formats). Non-Integration events cannot arrive (the subscription filters
   them out), but the consumer ignores them defensively.
5. **`LogPump`** — the same notify→post→drain pump with a tail-draining
   `stop()` as event_logger/mouse_snake_trail.

### Subscription

`SubscriptionScope{ .kinds = {}, .filter = { .categories = {Integration} } }`
— empty kinds expands to all registered kinds at subscribe time, the filter
narrows delivery to Integration-category events. `QueueOptions{ .capacity =
1024, .overflow = NeverDrop }` (browser events are low-rate).

### Data flow

browser extension → unix socket → `BrowserBridge` → session bus →
subscription (Integration filter) → notify → posted drain on session reactor →
`ScreenshotSaver::capture_and_save` → PNG file + stdout line.

Signals (SIGINT/SIGTERM) are blocked before `Session::open()`; the main thread
polls with `sigtimedwait`. No coalescer/tick — browser events are discrete and
infrequent.

## Filenames

`<dir>/<YYYYMMDD-HHMMSS-mmm>-<kind>.png`, e.g.
`browser-screenshots/20260719-190245-123-tab_changed.png`. `<kind>` is a short
label per Integration kind (`tab_changed`, `context_update`). Timestamp from
`event.timestamp` (epoch seconds — uniform after the earlier pre-fix) in local
time. Collisions within the same millisecond are avoided by appending a
`-N` suffix when the target already exists.

## Shutdown

Ctrl+C → poll loop notices → stop the browser socket → `pump.stop()` (tail
drain + fence) → `session->close()` → summary
(`N browser events, M screenshots saved, dir: <dir>`) → exit 0 (or 1 if a
capture error was remembered).

## Error handling

- Startup failures (session, `Screen::open`, socket, output-dir creation) →
  exit 1 with the `grab::Error` message.
- Runtime capture/encode/write failure → one stderr warning, remembered,
  observation continues, exit 1 at the end. Never aborts mid-run.
- `Screen::display()` returning an error (e.g. transient X failure) is treated
  as a runtime capture failure, not fatal.

## Testing / verification

- Build gate: `GRAB_BUILD_EXAMPLES` compiling clean under the default
  ASan/UBSan + clang-tidy preset.
- Unit-ish: `capture_filename` formatting is exercised by a tiny standalone
  assertion in the smoke script (or left to review — examples carry no GTest).
- Smoke script `examples/browser_event_screenshot_smoke.sh`: Xvfb + a canned
  native-messaging frame piped into the socket via python3, then assert a
  `.png` file appears in the output dir and is a non-empty PNG (magic bytes).
  Fully runnable here — `Screen::display()` works under Xvfb (the screengrab
  tests already rely on it), and no real browser is needed.
- `event_logger` regression: its existing smoke must still pass after the
  `BrowserSocket` extraction.

## Out of scope

- No per-browser window targeting (full display only), no image diffing, no
  video, no throttling/dedup of rapid tab switches (each event → one shot),
  no Wayland.
