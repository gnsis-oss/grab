# grab examples

Everything runnable in this directory, in one place. There are two kinds of
thing here: **the suite** (small, ordered, step-by-step sequence documents,
played by one runner) and **standalone demos** (older, each with its own entry
point).

## The suite — `run_suite.sh`

```bash
examples/run_suite.sh                  # play every example, first to last, on an
                                       # owned headless Xvfb + compositor
examples/run_suite.sh --local          # same, but on the CURRENT $DISPLAY —
                                       # starts no server, no WM, no container
examples/run_suite.sh 01_button_click  # one example, by name (01 and
                                       # button_click also match)
examples/run_suite.sh --list           # what exists
```

The suite is `suite/NN_*.json`, played in filename order. **Adding
`03_whatever.json` is the whole registration step** — the next full run plays
it after `02`. Every example runs with full debug logging (`--log-level debug`,
`--trace`), the pointer trail (`--trail`) and click feedback (`--feedback`),
and leaves its `grab.log`, `play.log`, `run.jsonl` and screenshots under
`run_suite_out/<example>/` (git-ignored).

| # | entry | what it demonstrates |
|---|---|---|
| 01 | `suite/01_button_click.json` | Navigate to one button and click it: a target rect appears, the planned route is drawn, the pointer follows it with a trail, clicks, the button flashes and turns green, screenshot. |
| 02 | `suite/02_three_buttons.json` | Three buttons scattered across the field (marked with 1, 2 and 3 dots), visited and clicked in order; each turns green as it is hit, screenshot at the end. |
| 03 | `suite/03_stage_button.sh` → `stage_button.cpp` | The same click against a **real page**: an authored HTML button in Firefox, resolved through the accessibility tree, approached on a human trajectory with a live trail, and the click proved three independent ways — the a11y name flips, the pixels flip, and the press point read back from the X server lands inside the button. 6-check scorecard. |

In 01 and 02 a "button" is an overlay shape the document itself draws — those
examples need no application under them, so they run identically on a bare
Xvfb and on a real desktop. 03 graduates to a real application: it is rung 4
of the visual capability ladder ported from the spider work
(`l0/.worktrees/spider-visual`), and `examples/support/` carries the pieces it
brought with it — `host.hpp` (private X session + WM + compositor + a11y bus +
Firefox, torn down by recorded PID), `motion/` (human trajectory synthesis)
and `stage/` (scene, probe and scorecard vocabulary for future rungs).

## Standalone demos

| entry point | what it is |
|---|---|
| `sequences/osu_stress.json` | The big sequence stress demo: 199 steps — streams, sliders, carries, a spinner. `grab play examples/sequences/osu_stress.json`. See `sequences/README.md`. |
| `mouse_snake_trail.cpp` | C++ API demo: a snake of overlay segments chasing the pointer. Built with `-DGRAB_BUILD_EXAMPLES=ON`. |
| `event_logger.cpp` + `event_logger_smoke.sh` | C++ API demo: subscribe to the observation stream and print events; the smoke script drives it headlessly. |
| `capture_windows_demo.sh` | Shell demo of `grab windows` + `grab capture` against an owned Xvfb. |
| `browser-bridge/` | The WebExtension native-messaging host experiment. Not part of the suite. |

`screenshots/` and `run_suite_out/` are output directories, both git-ignored.
