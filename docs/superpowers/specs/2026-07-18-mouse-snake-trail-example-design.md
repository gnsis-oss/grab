# mouse_snake_trail example — design

Date: 2026-07-18
Status: approved
Depends on: `2026-07-17-overlay-design.md` (Overlay Phase 1)

## Purpose

The first buildable C++ example for grab: `examples/mouse_snake_trail.cpp`.
It sweeps the pointer from the top-left of the screen to the bottom-right in a
boustrophedon (snake) pattern — sweep right, drop one row, sweep left, drop,
repeat — while a fading comet-tail trail follows the cursor.

The example demonstrates the full stack through **public API only**:

- **Synthesize** — `grab::Input::move` drives the pointer.
- **Observe** — `Session::watch` + `start_observation` deliver the resulting
  `MouseMove` events.
- **Render** — `grab::Overlay` (Phase 1) draws fading trail segments from the
  observed events.

The trail is observation-driven, not self-drawn: the program draws what it
*observes*, exactly like the built-in `grab overlay trail` feature, so the
example doubles as a readable reference for that loop.

## Architecture — one process, two roles

### Trail renderer (reactor side)

1. `Session::open()`; `session->overlay()` for the `Overlay*` facade;
   `overlay->space()` for the `CoordinateSpaceId` to stamp on all points.
2. `session->watch(SubscriptionScope{ .kinds = { EventKind::MouseMove } })`.
3. `subscription.set_notify(...)` posts a drain job onto the session reactor
   via `session->post()` — the notify→post→drain pattern the CLI trail command
   uses, simplified for example readability (redundant posts are harmless; the
   drain just finds an empty queue).
4. The drain keeps the last observed position. For each `MouseMove` event whose
   payload carries an absolute `position`, it adds a segment shape:
   - geometry: `Path{ MoveTo(previous), LineTo(current) }`
   - `Band::Trail`, `z = 0`
   - lifetime: `Fade{ 1200ms }` (comet tail)
   - stroke: 3.0 px; color by `Event::origin` using the built-in palette —
     `Physical` red, otherwise blue (matching the kernel trail's ternary).
     Wiggling the physical mouse during a run draws a red trail beside the
     blue snake.
   Adds alone trigger presentation (the kernel's own trail path never flushes
   per segment); the example calls `overlay->flush()` once from the main
   thread after the sweep, as a visibility fence before the fade hold.
5. Events without an absolute position (delta-only, e.g. evdev fallback) and
   `QueueGapMarker` items break the path (reset `previous`) instead of drawing.

### Snake driver (main thread)

1. `Screen::open()` + `display()` — the captured image's width/height give the
   sweep bounds (the public-API way to size the screen; one capture at start).
2. Waypoints: inset all edges by a margin; rows spaced so the sweep has an
   **odd** number of horizontal passes, guaranteeing the final pass travels
   left→right and ends at the bottom-right corner of the inset area.
3. `Input::open()`; each pass is interpolated in fixed pixel steps with a short
   sleep per step (~2000 px/s overall). Row drops are interpolated the same way.
4. On completion: hold one fade duration so the tail evaporates on screen, then
   `stop_observation()` and exit 0.

The Session runs its reactor on an internal thread, so the main thread is free
to sleep-step the movement while observation, drain, and rendering proceed
concurrently.

### Rejected alternative

Reusing the kernel's `TrailAnimator`/`OverlayScene` (as the CLI does) was
rejected: those are internal headers under `src/kernel/presentation/`, and an
example must teach the public surface. The hand-rolled segment loop is ~40
lines and shows exactly how a trail is built from events.

## Build wiring

- New `examples/CMakeLists.txt` with
  `add_executable(mouse_snake_trail mouse_snake_trail.cpp)`, linking the same
  static targets the CLI links: `grab_core`, `grab_kernel`, `grab_session`,
  `grab_platform_x11`, `grab_input`, `grab_input_x11`, `grab_screen`,
  `grab_image`, `grab_codec` (trimmed to the minimal working set during
  implementation).
- Root `CMakeLists.txt` gains `option(GRAB_BUILD_EXAMPLES "..." ON)` and a
  guarded `add_subdirectory(examples)`.
- Same regime as the rest of the tree: C++23, warnings-as-errors, clang-format,
  clang-tidy on every build.

## Error handling

All `Result` values are checked. Any failure prints the error message to
stderr and exits non-zero. Overlay `add`/`flush` failures inside the drain are
remembered (first error wins) and reported at exit, mirroring the CLI pattern.

## Configuration

No CLI flags (YAGNI). Geometry, speed, and style are named `constexpr`
constants at the top of the file — margin, row-pass count target, step size,
step interval, fade duration, stroke width, palette — where a reader can tweak
them.

## Verification

1. Clean build with examples enabled (format + tidy + warnings-as-errors).
2. Run under Xvfb; capture the display mid-run and confirm trail segments
   render along the snake path.
3. Live run on a real display as the demo — the program genuinely moves the
   pointer; that is its purpose. Use the release preset: the sanitized dev
   build renders the overlay at ~5 fps on large displays, which starves the
   reactor and makes the trail look static.

## Out of scope

- Wayland (overlay Phase 1 is X11).
- CLI flags / argument parsing.
- Reuse from other examples (this is the first one); no shared example
  scaffolding until a second example needs it.
