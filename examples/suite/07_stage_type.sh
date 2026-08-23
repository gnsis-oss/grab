#!/usr/bin/env bash
# Suite wrapper for stage_type — rung 6 of the visual capability ladder:
# focus a labelled MESSAGE field with a real click, type a paragraph letter
# by letter at a human rhythm (word breaths, comma and full-stop beats,
# occasional thinks), then press SEND. The a11y value must equal the
# paragraph exactly, the receipt must carry what the page's script received,
# and the X server must have seen a key-down per character. See
# examples/stage_type.cpp.
#
# Run by examples/run_suite.sh with cwd = this example's artifact directory
# and SUITE_MODE / SUITE_BUILD / DISPLAY set. Same mode mapping as the other
# stage wrappers: owned = its own nested stack, local = --session.
set -uo pipefail

BIN="${SUITE_BUILD:?run via run_suite.sh}/examples/stage_type"
[ -x "$BIN" ] || {
  echo "stage_type is not built at $BIN — build the preset first" >&2
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
