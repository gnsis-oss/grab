#!/usr/bin/env bash
# Drive the overlay features on an OWNED headless X display and capture frames.
#
#   scripts/overlay-demo.sh [DISPLAY_NUM] [OUT_DIR]
#
# Never touches the live session: it starts its own Xvfb plus a compositing
# manager (the overlay needs ARGB compositing and refuses to map without it),
# runs the demo, captures PNGs, and tears everything down.
set -uo pipefail

DISPLAY_NUM="${1:-95}"
OUT_DIR="${2:-$PWD/overlay-demo-out}"
BUILD="${BUILD:-build}"
GRAB="$BUILD/grab"
SCREEN="1280x1024x24"

[ -x "$GRAB" ] || { echo "no $GRAB -- run: ninja -C $BUILD" >&2; exit 1; }
command -v Xvfb >/dev/null || { echo "Xvfb not installed" >&2; exit 1; }
COMPOSITOR="$(command -v xcompmgr || command -v picom || true)"
[ -n "$COMPOSITOR" ] || { echo "need xcompmgr or picom" >&2; exit 1; }

mkdir -p "$OUT_DIR"
export ASAN_OPTIONS=detect_leaks=0
export GCOV_PREFIX=/tmp/grab-demo-gcov     # keep coverage spew out of the tree
D=":$DISPLAY_NUM"

xvfb_pid=""; comp_pid=""; trail_pid=""
cleanup() {
  for pid in "$trail_pid" "$comp_pid" "$xvfb_pid"; do
    [ -n "$pid" ] && kill -TERM "$pid" 2>/dev/null
  done
}
trap cleanup EXIT

Xvfb "$D" -screen 0 "$SCREEN" -noreset >/dev/null 2>&1 & xvfb_pid=$!
sleep 2
xdpyinfo -display "$D" >/dev/null 2>&1 || { echo "Xvfb $D failed to start" >&2; exit 1; }

DISPLAY="$D" "$COMPOSITOR" >/dev/null 2>&1 & comp_pid=$!
sleep 2
echo "display $D  xvfb=$xvfb_pid compositor=$comp_pid  -> $OUT_DIR"

shot() { DISPLAY="$D" "$GRAB" capture --display --out "$OUT_DIR/$1" 2>/dev/null \
         | grep -v '^profiling:' || true; }
drag() { DISPLAY="$D" "$GRAB" drag --from "$1" --to "$2" >/dev/null 2>&1; }
click(){ DISPLAY="$D" "$GRAB" click --at "$1" >/dev/null 2>&1; }

# --- feature 1: cursor trail -------------------------------------------------
echo "[1/2] trail"
DISPLAY="$D" "$GRAB" trail --fade-ms 8000 --width 8 >/dev/null 2>&1 & trail_pid=$!
sleep 3
for _ in 1 2 3; do
  drag "200,300" "900,300"     # horizontal stroke
  drag "900,300" "900,700"     # vertical stroke
done
sleep 1
shot "01-trail.png"
kill -TERM "$trail_pid" 2>/dev/null; trail_pid=""; sleep 1

# --- features 2 & 4: click ripple and hold bar -------------------------------
# Driven through the library, not the CLI: cursor feedback has no verb yet.
echo "[2/2] ripple + hold  (needs examples/overlay_showcase -- not built yet)"
if [ -x "$BUILD/examples/overlay_showcase" ]; then
  DISPLAY="$D" "$BUILD/examples/overlay_showcase" >/dev/null 2>&1 &
  sleep 2; click "500,400"; sleep 1; shot "02-ripple.png"
else
  echo "     SKIPPED: build it, or drive grab::Session::cursor_feedback() yourself"
fi

echo
echo "frames written:"
ls -1 "$OUT_DIR" 2>/dev/null | sed 's/^/  /'
