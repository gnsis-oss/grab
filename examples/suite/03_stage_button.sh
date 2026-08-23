#!/usr/bin/env bash
# Suite wrapper for stage_button — rung 4 of the visual capability ladder:
# a real Firefox page with one button, a human trajectory with a live trail,
# and the click proved three independent ways (a11y name flip, pixel flip,
# device-read press point). See examples/stage_button.cpp.
#
# Run by examples/run_suite.sh with cwd = this example's artifact directory
# and SUITE_MODE / SUITE_BUILD / DISPLAY set:
#   owned  — stage_button creates its OWN nested stack (Xvfb + openbox +
#            xcompmgr + dbus + a11y bus + Firefox) on a free display; the
#            runner's bare Xvfb has no window manager, and without one
#            Firefox never takes input focus.
#   local  — --session: drives the display you are already on, starting no
#            X server, WM or compositor, only its own a11y bus + Firefox.
set -uo pipefail

BIN="${SUITE_BUILD:?run via run_suite.sh}/examples/stage_button"
[ -x "$BIN" ] || {
  echo "stage_button is not built at $BIN — build the preset first" >&2
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
