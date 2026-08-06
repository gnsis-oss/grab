# Sequence examples

A grab **sequence** is a JSON document played by `grab play`. `osu_stress.json`
is one, and it is the whole example — **one document, one command, one process**:

```bash
grab play examples/sequences/osu_stress.json
```

## It used to be two binaries

Until the eight `overlay.*` steps existed, the `Command` variant covered input,
capture and wait only. A sequence could move the pointer onto a target and click
it but **could not place, move or remove the target**, so anything visual needed
a companion C++ program — `examples/osu_stress.cpp` — reading the same document
and drawing what the pointer was aiming at. Two executables for one example, a
start order to get right, and grab's most distinctive capability reachable only
from C++.

That program is deleted. `osu_stress.json` now carries its own visuals:

| step | what it draws |
|---|---|
| `overlay.add` ellipse | a hit circle, one per target, removed after its click |
| `overlay.add` ellipse (r=78) | the approach ring on the star-jump and finale targets |
| `overlay.add` path | the **requested mouse line** — the exact Bézier each `input.follow` walks |
| `overlay.add` rect + `overlay.attach` | the draggable square, which rides the pointer through both carries |
| `overlay.add` with `ttl_ms` | the hit flash: no handle, no removal, expired by the scene itself |
| `overlay.grab` / `overlay.release` | the pointer capture the carry needs |
| `overlay.clear` | the teardown, just before the final screenshot |

199 steps. Every coordinate is written once, in one file, so there is nothing
left to drift.

## Reading and editing it

Steps run top to bottom: a step with no `after` depends on the one before it.
The ids carry the structure, and the groups are separated by blank lines:

```
field-frame          the playfield border
sweep-*  line-*      an opening sweep, drawn with a fade lifetime
c01..c08  hit-01..08 the stream: add circle, move, click, remove circle
c09..c13  hit-09..13 star jumps, with approach rings and a ttl flash per hit
routeA   slider-a-*  a slider: draw the route, press, follow it, release
c14..c18  hit-14..18 corner jumps
sq       square-*    carry A: grab the pointer, attach the square, carry, detach
sq       square2-*   carry B: the same square, picked up again where it landed
spinring spin-*      the spinner: three arcs with the button down
c19..c24  hit-19..24 the finale, with left, right and middle buttons
wheel-*              three scroll bursts
chord-*              Ctrl+A, which needs input.key_down / input.key_up
outro-*  field-clear the closing sweep, the teardown, the screenshot
```

To move a target, change one `to` and the matching `center`. To retime it,
change `step_dwell_ms` or the `pacing` block — or leave the document alone and
pass `--pacing strict` to find the tightest timing your machine tolerates.

### Two rules the document has to respect

- **A handle is live between its `overlay.add` and its `overlay.remove`.** Using
  one before it is added, or adding it again while it is still live, is a
  *loader* error with a JSON pointer at it. Reusing a name after a remove is
  fine — that is a new shape.
- **A press is indistinguishable from a click at the X level.** Keep every
  `input.press` and `input.click` position more than a hit radius away from
  every circle centre it is not aiming at, or the press strikes that circle
  early. The tightest separation in this document is 70.7 px against a 48 px
  radius.

## Build

Examples are gated on `GRAB_BUILD_EXAMPLES` (default ON), but this one needs no
example target at all — only the `grab` CLI.

```bash
cmake -B build/osu -G Ninja \
      -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DGRAB_FORMAT=OFF -DGRAB_TIDY=OFF
cmake --build build/osu --target grab
```

## Run it

Never on your live display: it takes the pointer capture and drives the mouse.

The overlay needs a **compositing manager** (ARGB32, XFixes and an owned
`_NET_WM_CM_Sn`), so a bare Xvfb is not enough — start `xcompmgr` or `picom` on
it too. Without one, the `overlay.add` steps fail and the run aborts; the input
half alone still works if you delete the overlay steps.

```bash
# 1. an owned headless display, PIDs captured so they can be killed BY PID
Xvfb :137 -screen 0 1920x1080x24 -noreset & XVFB_PID=$!
sleep 2
DISPLAY=:137 xcompmgr & COMP_PID=$!

# 2. the whole example
DISPLAY=:137 ./build/osu/grab play examples/sequences/osu_stress.json

# 3. tear down by captured PID -- never by pattern
kill "$COMP_PID"; kill "$XVFB_PID"
```

Useful flags:

```
--dry-run             print the plan and the step order; execute nothing
--pacing strict       ignore the document's grace and run back to back
--grace-ms N          change the gap without changing which mode reads it
--report run.jsonl    one JSON record per step: status, call_ns, overrun_ns
--log-level verbose --log-tags player,frame,raster
```

## Interrupting it

`grab play` traps SIGINT and SIGTERM and turns them into an unwind rather than a
dead process. That matters here because of `overlay.grab`: **a pointer grab that
outlives its owner freezes the whole desktop**, recoverable only through another
X client or a VT switch. Ctrl-C mid-carry runs the same unwind an abort does, so
the capture and the held button come up first, and the run exits non-zero.

Two independent mechanisms guarantee it, and neither is redundant:

1. `Player::unwind` reaches back into the already-completed `overlay.grab` step
   through `CommandRunner::release_holds` and lifts the capture there — while
   the run is tearing down, not after the report has been written.
2. The CLI's runner tracks the capture beside its held buttons and keys and
   lifts whatever is still outstanding on **every** exit path, including the one
   no unwind reaches: a run that completes normally with the document having
   simply forgotten its `overlay.release`.

## What it demonstrates

- **All eight overlay steps** — `add`, `update`, `remove`, `clear`, `grab`,
  `release`, `attach`, `detach` — driving a real `grab::Overlay`.
- **`overlay.attach`**, which is what makes a carry look like a carry. The seat
  sees every waypoint of an `input.move`, so an attached shape is repositioned
  on each of them instead of teleporting at the end. Its offset defaults to the
  shape's position minus the pointer's at attach time, so a square picked up by
  its corner stays held by that corner.
- **Lifetime**: `ttl_ms` on the hit flashes and `fade_ms` on the opening sweep.
  A shape with either is expired **by the scene itself**, so the later
  `overlay.remove` on `sweep` may find nothing — that is a documented no-op, not
  an error, which is what makes "a fading flash plus explicit cleanup" writable.
- **Fire-and-forget adds**: the flashes carry no handle. Drawable, never
  referenced again.
- **Mixed buttons** (left, right, middle), **scroll notches**, and a **Ctrl+A
  chord**, which is only expressible because `input.key_down` / `input.key_up`
  exist as steps.
