#!/usr/bin/env bash
# Suite wrapper for stage_drag — rung 8 of the visual capability ladder:
# grab an authored app icon in a real Firefox, carry it across the page with
# the button held on a human trajectory, drop it into a zone — the
# macOS-install gesture — and verify on a11y, pixel and device channels.
# See examples/stage_drag.cpp.
#
# Run by examples/run_suite.sh with cwd = this example's artifact directory
# and SUITE_MODE / SUITE_BUILD / DISPLAY set. Same mode mapping as
# 03_stage_button.sh: owned = its own nested stack (the runner's bare Xvfb
# has no WM), local = --session on the current display.
set -uo pipefail

BIN="${SUITE_BUILD:?run via run_suite.sh}/examples/stage_drag"
[ -x "$BIN" ] || {
  echo "stage_drag is not built at $BIN — build the preset first" >&2
  exit 1
}

if [ "${SUITE_MODE:?}" = "local" ]; then
  exec "$BIN" --session --trail --out "$PWD"
fi

# Own display, outside the runner's 73-86 range and the test fixture's 87-99.
D=""
for n in $(seq 60 72); do
  [ -e "/tmp/.X11-unix/X$n" ] || { D=":$n"; break; }
done
[ -n "$D" ] || { echo "no free display number in 60-72" >&2; exit 1; }
exec "$BIN" --display "$D" --trail --out "$PWD"
