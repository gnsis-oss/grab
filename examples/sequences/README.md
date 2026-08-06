# Sequence examples

A grab **sequence** is a JSON document played by `grab play`. `osu_stress.json`
is one, and it ships with a visual counterpart — `examples/osu_stress.cpp` —
that is meant to be run *alongside* it.

```
osu_stress.json   the 90-step sequence: warps, moves, clicks, presses,
                  curve follows, a spin, scrolls and a Ctrl+A chord
                  across a 1920x1080 playfield.        <- the INPUT half

osu_stress.cpp    draws the hit circles, approach rings, curve routes and
                  the draggable square those steps aim at, and removes each
                  target when a click is OBSERVED on it.  <- the VISUAL half
```

## Why they are a pair and not one program

The two halves share exactly one thing: **the document**. `osu_stress` parses
`osu_stress.json` and derives every coordinate it draws from it —

| what it draws | where it comes from |
|---|---|
| hit circle + approach ring | every step whose `id` is `hit-NN`, centred on that step's `to` |
| curve route (the mouse-line) | every step with `"op": "input.follow"`, from its `curve` control points |
| draggable square | the `to` of the **first** step whose `id` starts `square-` or `square2-` |

Nothing is hardcoded. Move a target in the JSON and the visual follows it with
no rebuild of intent — the two halves cannot drift apart, because there is only
one copy of the numbers.

Removal is caused by the hit, never scheduled next to it: a circle disappears
because a `MouseButtonDown` arrived from the observation stream inside its
radius. Retime the sequence and the visuals stay correct. **A circle that is
never struck stays on screen** — that is the failure signal, not a bug.

## Build

Examples are gated on `GRAB_BUILD_EXAMPLES` (default ON).

```bash
cmake -B build/osu -G Ninja \
      -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DGRAB_FORMAT=OFF -DGRAB_TIDY=OFF
cmake --build build/osu --target osu_stress grab
```

## Run the pair

Never on your live display. The overlay needs a compositing manager (ARGB32,
XFixes and an owned `_NET_WM_CM_Sn`), so a bare Xvfb is not enough: start
`xcompmgr` or `picom` on it too.

```bash
# 1. an owned headless display, PID captured so it can be killed by PID
Xvfb :131 -screen 0 1920x1080x24 -noreset & XVFB_PID=$!
DISPLAY=:131 xcompmgr & COMP_PID=$!

# 2. the visual half. --hold 0 keeps it up until SIGINT/SIGTERM.
./build/osu/examples/osu_stress --display :131 --hold 0 & OSU_PID=$!

# 3. the input half, on the same display
DISPLAY=:131 ./build/osu/grab play examples/sequences/osu_stress.json

# 4. tear down by captured PID -- never by pattern
kill -INT "$OSU_PID"; kill "$COMP_PID"; kill "$XVFB_PID"
```

`osu_stress` prints its stress report on the way out: shapes added and removed,
peak concurrent shapes, hits, misses, queue gaps, the slowest single overlay
call, and a per-verb cost table.

Either half runs alone. Started by itself, `osu_stress` places the field and
waits, so a human can look at it; `grab play` replays the pointer with nothing
drawn under it.

## `osu_stress` options

```
osu_stress [--document PATH] [--display :N] [--hold SECONDS]

  --document PATH   sequence document to derive the field from
                    (default: this directory's osu_stress.json)
  --display :N      X display to open the session on
  --hold SECONDS    how long to keep the field up; 0 waits for a signal
```

## What it demonstrates

- **`Overlay::add_many`** for the initial field. The round trip is priced per
  *call*, not per shape, so the ~56 shapes of a fresh playfield go in as one.
- **`Overlay::capture_pointer` / `release_pointer`** for the carry section. The
  capture is armed when the pointer **enters** the square — the moment the carry
  tool becomes armed — never at the button press, which by then has already been
  delivered to whatever is underneath. The release is in a destructor and runs
  on every exit path, including the SIGINT one: a pointer grab that outlives its
  owner freezes the whole desktop.
- **`Session::watch`** with a reactor-thread drain. `MouseButtonDown` is a
  `NeverDrop` event kind, so a queue overrun would cost an unobserved click; the
  report counts queue gaps so that never passes silently.
