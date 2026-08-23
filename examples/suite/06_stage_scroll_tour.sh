#!/usr/bin/env bash
# Suite wrapper for stage_scroll_tour — rung 9, long form: a variable-speed
# scroll journey. Down slowly to a bottom button, back up at walking pace to
# a midway one, down again on a ramp to a third — clicking each stop, with
# each leg's necessity asserted before it runs and wheel events in BOTH
# directions witnessed in the X server's stream. See stage_scroll_tour.cpp.
#
# Run by examples/run_suite.sh with cwd = this example's artifact directory
# and SUITE_MODE / SUITE_BUILD / DISPLAY set. Same mode mapping as the other
# stage wrappers: owned = its own nested stack, local = --session.
set -uo pipefail

BIN="${SUITE_BUILD:?run via run_suite.sh}/examples/stage_scroll_tour"
[ -x "$BIN" ] || {
  echo "stage_scroll_tour is not built at $BIN — build the preset first" >&2
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
