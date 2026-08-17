#!/usr/bin/env bash
# Suite wrapper for stage_scroll — rung 9 of the visual capability ladder:
# a four-screenful page whose button starts below the fold, is wheeled into
# view in measured bursts (re-reading the live a11y rect between rounds),
# then approached and clicked, with the wheel notches and the click pair
# both witnessed in the X server's event stream. See examples/stage_scroll.cpp.
#
# Run by examples/run_suite.sh with cwd = this example's artifact directory
# and SUITE_MODE / SUITE_BUILD / DISPLAY set. Same mode mapping as the other
# stage wrappers: owned = its own nested stack, local = --session.
set -uo pipefail

BIN="${SUITE_BUILD:?run via run_suite.sh}/examples/stage_scroll"
[ -x "$BIN" ] || {
  echo "stage_scroll is not built at $BIN — build the preset first" >&2
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
