---
name: grab
description: >-
  Drive a Linux/X11 desktop with the grab CLI — take window screenshots, list
  and focus/move windows, and synthesize mouse clicks, drags, and typed text.
  Use when the user wants to screenshot a window or region, inspect what windows
  are open, focus/raise/place a window, click or type into an application,
  automate a GUI, do visual testing, or observe keyboard/mouse/focus events.
---

# grab

`grab` is a single C++ library and CLI for OS-level desktop interaction on
**Linux/X11**. It does three things: **observe** input, **capture** output, and
**synthesize** input. Everything runs against the live X display, so it must
execute on the machine with the desktop session (or a virtual `Xvfb` display).

Check availability first with `grab doctor --json`; it reports which backends
and capabilities are present. If `grab` is not on `PATH`, build it from the repo
(`cmake --preset default && cmake --build build`) and use `build/bin/grab`.

## Prefer structured output

Pass `--json` where it exists — today that is `doctor`, `windows`, and
`watch status` — and parse that rather than scraping text. Other verbs
(`capture`, `compare`, `batch`) print human-readable output. Every verb supports
`grab <verb> --help`; read it before guessing flags.

## Capture output (read-only, safe)

```bash
grab capture --window "Firefox" --out shot.png    # matched window → PNG
grab capture --window-id 0x1e00007 --out shot.png # a specific window id
grab capture --region 0,0,1280x720 --out crop.png # a region "X,Y,WxH"
grab capture --display --out full.png             # the whole display (flag, no value)
grab windows --json                               # enumerate windows (id, class, title, type, pid, bounds)
grab compare a.png b.png                           # match_ratio + diff_pixels (text)
grab watch --window "Slack" --out slack.png        # re-capture the window whenever it changes
grab doctor --json                                # environment & capability report
```

`grab windows --json` is the usual first step: it gives you the window **id**
and **wm_class** you then pass to capture / focus / place / input. Note `watch`
is a screenshot-on-change watcher, not an input-event observer — observing
keyboard/mouse/focus events is the job of `grab daemon`.

## Control windows (mutating, converges to a fixed state)

```bash
grab focus --window "Firefox"          # raise + focus by WM_CLASS substring
grab focus --window-id 0x1e00007       # raise + focus by native id
grab place --window-id 0x1e00007 --geometry 1280x720+100+100  # WxH+X+Y; waits until held
```

## Synthesize input (mutating — acts on the real desktop)

```bash
grab click --at 640,360                 # click at screen coordinates (--button N, numeric)
grab click --locator "button:Submit"    # click a resolved UI target
grab type --text "hello world"          # type text into the focused surface
grab key --window-id 0x1e00007 --keysym Return  # send one keysym (name, not a chord)
grab drag --from 100,100 --to 400,400   # straight drag
grab drag-curve --window "Canvas" --src 100,100 --dst 400,400  # curved/gesture drag
```

## Safe-by-default workflow

1. `grab doctor --json` — confirm the display and capabilities are available.
2. `grab windows --json` — find the target window's id / class.
3. Read-only step (`capture`) to see current state.
4. Only then a mutating step (`focus`, `place`, `click`, `type`, `key`), and
   re-`capture` to verify the effect.

Input synthesis moves the real pointer and types real keystrokes — never run a
mutating verb speculatively. When unsure of coordinates, `capture` and inspect
first. On a headless box, start a virtual display (`Xvfb`) and point `grab` at
it with `--display`.

## Callable tools (optional)

If the `grab-mcp` MCP server is configured, the same operations are exposed as
typed tools (`capture_screen`, `list_windows`, `focus_window`, `click`,
`type_text`, …) with JSON schemas — prefer those over shelling out when they are
available. See `mcp/README.md` and `mcp/tools.json` in the grab repo.
