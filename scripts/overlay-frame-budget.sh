#!/usr/bin/env bash
# Measure the WHOLE overlay frame pipeline on an OWNED headless display.
#
#   scripts/overlay-frame-budget.sh [DISPLAY_NUM] [OUT_DIR]
#
# benchmarks/overlay_raster_bench prices the rasterizer with no X server in
# sight, which is what makes a change attributable. It cannot tell you what a
# frame costs once conversion, presentation and the compositor are in it. This
# does, by running the real CLI verbs against its own Xvfb and reading back
# grab's own per-frame instrument.
#
# Never touches the live session: its own Xvfb, its own compositing manager
# (the overlay needs ARGB compositing and refuses to map without one), torn
# down on the way out.
set -uo pipefail

DISPLAY_NUM="${1:-96}"
OUT_DIR="${2:-$PWD/overlay-frame-budget-out}"
BUILD="${BUILD:-build/dev}"
GRAB="$BUILD/grab"
# Large enough that a full-surface annotation is a real pixel load.
SCREEN="${SCREEN:-1920x1200x24}"

[ -x "$GRAB" ] || { echo "no $GRAB -- run: cmake --preset ${BUILD##*/} && cmake --build --preset ${BUILD##*/}" >&2; exit 1; }
command -v Xvfb >/dev/null || { echo "Xvfb not installed" >&2; exit 1; }
COMPOSITOR="$(command -v xcompmgr || command -v picom || true)"
[ -n "$COMPOSITOR" ] || { echo "need xcompmgr or picom" >&2; exit 1; }

mkdir -p "$OUT_DIR"
D=":$DISPLAY_NUM"

xvfb_pid=""; comp_pid=""; worker_pid=""
cleanup() {
  for pid in "$worker_pid" "$comp_pid" "$xvfb_pid"; do
    [ -n "$pid" ] && kill -TERM "$pid" 2>/dev/null
  done
}
trap cleanup EXIT

Xvfb "$D" -screen 0 "$SCREEN" -noreset >/dev/null 2>&1 & xvfb_pid=$!
# Poll rather than sleep a fixed span: Xvfb compiles a keymap before it
# answers, and on a loaded machine that takes longer than any constant worth
# hard-coding.
for _ in $(seq 40); do
  xdpyinfo -display "$D" >/dev/null 2>&1 && break
  sleep 0.25
done
xdpyinfo -display "$D" >/dev/null 2>&1 || { echo "Xvfb $D failed to start" >&2; exit 1; }
DISPLAY="$D" "$COMPOSITOR" >/dev/null 2>&1 & comp_pid=$!
# The overlay refuses to map until a compositing manager owns _NET_WM_CM_Sn.
for _ in $(seq 40); do
  DISPLAY="$D" xprop -root _NET_SUPPORTED >/dev/null 2>&1 && break
  sleep 0.25
done
sleep 1
echo "display $D  screen $SCREEN  build=$BUILD  -> $OUT_DIR"

# Reports every 120 frames under the `frame` tag: raster, convert, present and
# flush quantiles, plus how many frames missed the budget.
LOGENV=(--log-level verbose --log-tags frame,raster)

report() {
  local label="$1" file="$2"
  echo
  echo "── $label ──"
  grep -h 'tag=frame' "$file" | tail -1 || echo "  (no frame report -- fewer than 120 frames were presented)"
}

# ── 1. fast pointer ─────────────────────────────────────────
# One live shape per motion sample, all of them fading at once, is what a fast
# pointer does to the trail.
echo "[1/2] trail under a fast pointer"
DISPLAY="$D" "$GRAB" "${LOGENV[@]}" --log-file "$OUT_DIR/trail.log" \
  trail --fade-ms 4000 --width 3 >/dev/null 2>&1 & worker_pid=$!
sleep 2
# `drag` interpolates, so each call is a burst of motion samples and each
# sample is one live trail shape. Enough passes to clear the 120-frame report
# cadence with a few hundred segments alive at once.
for _ in $(seq 8); do
  DISPLAY="$D" "$GRAB" drag --from "200,200" --to "1700,1100" >/dev/null 2>&1
  DISPLAY="$D" "$GRAB" drag --from "1700,1100" --to "200,1100" >/dev/null 2>&1
  DISPLAY="$D" "$GRAB" drag --from "200,1100" --to "1700,200" >/dev/null 2>&1
done
sleep 1
DISPLAY="$D" "$GRAB" capture --display --out "$OUT_DIR/01-trail.png" >/dev/null 2>&1
kill -TERM "$worker_pid" 2>/dev/null; worker_pid=""; sleep 1
report "trail, fast pointer" "$OUT_DIR/trail.log"

# ── 2. large annotation ─────────────────────────────────────
# One shape covering most of the surface: the pixel-bound regime.
echo "[2/2] a near-full-surface annotation"
DISPLAY="$D" "$GRAB" "${LOGENV[@]}" --log-file "$OUT_DIR/overlay.log" \
  overlay rect --at "40,40,1840,1120" --hold >/dev/null 2>&1 & worker_pid=$!
sleep 4
DISPLAY="$D" "$GRAB" capture --display --out "$OUT_DIR/02-large.png" >/dev/null 2>&1
kill -TERM "$worker_pid" 2>/dev/null; worker_pid=""; sleep 1
report "large annotation" "$OUT_DIR/overlay.log"

echo
echo "frames written:"
ls -1 "$OUT_DIR" 2>/dev/null | sed 's/^/  /'
