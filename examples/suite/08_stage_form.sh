#!/usr/bin/env bash
# Suite wrapper for stage_form — rung 7 of the visual capability ladder:
# a form carrying every HTML input type (all 22) plus a combo box. The
# useful ones are driven for real — text, checkbox, radio, combo (by
# keyboard), range (click the track), number, date, password, and a real
# type=submit — with values verified through page-side mirrors read over
# a11y, an APPLY summary of everything the page's script received, and a
# JS-enumerated catalog proving the DOM carries all 22 types. See
# examples/stage_form.cpp.
#
# Run by examples/run_suite.sh with cwd = this example's artifact directory
# and SUITE_MODE / SUITE_BUILD / DISPLAY set. Same mode mapping as the other
# stage wrappers: owned = its own nested stack, local = --session.
set -uo pipefail

BIN="${SUITE_BUILD:?run via run_suite.sh}/examples/stage_form"
[ -x "$BIN" ] || {
  echo "stage_form is not built at $BIN — build the preset first" >&2
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
