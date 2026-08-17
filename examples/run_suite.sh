#!/usr/bin/env bash
# run_suite.sh — play the example suite end to end, or one example by name.
#
#   examples/run_suite.sh                  # every example, on an OWNED headless
#                                          # Xvfb + compositor it starts itself
#   examples/run_suite.sh --local          # every example, on the CURRENT
#                                          # $DISPLAY — starts no server, no WM,
#                                          # no compositor, no container
#   examples/run_suite.sh 01_button_click  # one example by name
#   examples/run_suite.sh 02               # ...or by number
#   examples/run_suite.sh --local 01       # modes combine
#   examples/run_suite.sh --list           # list the suite and exit
#
# The suite is examples/suite/NN_*.json, played in filename order — adding
# 03_whatever.json makes it part of every full run, no registration step.
#
# Every example runs with:
#   - full debug logging   (--log-level debug --log-file grab.log, plus --trace)
#   - the pointer trail    (--trail)
#   - click feedback       (--feedback)
# and leaves everything it produced under examples/run_suite_out/<example>/:
# grab.log, play.log, run.jsonl, and any screenshots the document captures.
#
# GRAB_BIN overrides binary resolution; otherwise the newest of
# build/{dev,release}/grab in this checkout wins, then `grab` on PATH.
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
SUITE_DIR="$SCRIPT_DIR/suite"
OUT_ROOT="$SCRIPT_DIR/run_suite_out"
SCREEN="1600x900x24"

LOCAL=0
LIST=0
PICK=""
for arg in "$@"; do
  case "$arg" in
    --local) LOCAL=1 ;;
    --list)  LIST=1 ;;
    --help|-h)
      sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    --*) echo "unknown flag: $arg" >&2; exit 2 ;;
    *)
      [ -n "$PICK" ] && { echo "only one example name may be given" >&2; exit 2; }
      PICK="$arg" ;;
  esac
done

# ── Discover the suite ──────────────────────────────────────────────────────
mapfile -t DOCS < <(find "$SUITE_DIR" -maxdepth 1 -name '[0-9][0-9]_*.json' | sort)
[ "${#DOCS[@]}" -gt 0 ] || { echo "no examples found in $SUITE_DIR" >&2; exit 1; }

if [ "$LIST" -eq 1 ]; then
  for doc in "${DOCS[@]}"; do basename "$doc" .json; done
  exit 0
fi

if [ -n "$PICK" ]; then
  MATCHED=()
  for doc in "${DOCS[@]}"; do
    stem="$(basename "$doc" .json)"
    num="${stem%%_*}"
    name="${stem#*_}"
    if [ "$PICK" = "$stem" ] || [ "$PICK" = "$num" ] || [ "$PICK" = "$name" ]; then
      MATCHED+=("$doc")
    fi
  done
  [ "${#MATCHED[@]}" -eq 1 ] || {
    echo "no unique example matches '$PICK' — try one of:" >&2
    for doc in "${DOCS[@]}"; do basename "$doc" .json >&2; done
    exit 2
  }
  DOCS=("${MATCHED[@]}")
fi

# ── Resolve the grab binary ─────────────────────────────────────────────────
GRAB="${GRAB_BIN:-}"
if [ -z "$GRAB" ]; then
  newest=""
  for cand in "$REPO_ROOT/build/dev/grab" "$REPO_ROOT/build/release/grab"; do
    [ -x "$cand" ] || continue
    if [ -z "$newest" ] || [ "$cand" -nt "$newest" ]; then newest="$cand"; fi
  done
  GRAB="${newest:-$(command -v grab || true)}"
fi
[ -n "$GRAB" ] && [ -x "$GRAB" ] || {
  echo "no grab binary: set GRAB_BIN, or build one (cmake --preset dev && cmake --build --preset dev)" >&2
  exit 1
}

# ── Display: current session (--local) or an owned Xvfb + compositor ────────
xvfb_pid=""; comp_pid=""
cleanup() {
  # PIDs only, captured at launch — never kill by pattern.
  [ -n "$comp_pid" ] && kill -TERM "$comp_pid" 2>/dev/null
  [ -n "$xvfb_pid" ] && kill -TERM "$xvfb_pid" 2>/dev/null
}
trap cleanup EXIT

if [ "$LOCAL" -eq 1 ]; then
  [ -n "${DISPLAY:-}" ] || { echo "--local needs \$DISPLAY set" >&2; exit 1; }
  echo "suite: local mode on $DISPLAY (no Xvfb, no WM, no compositor started)"
else
  command -v Xvfb >/dev/null || { echo "Xvfb not installed" >&2; exit 1; }
  COMPOSITOR="$(command -v xcompmgr || command -v picom || true)"
  [ -n "$COMPOSITOR" ] || { echo "need xcompmgr or picom (the overlay needs compositing)" >&2; exit 1; }

  # Pick a free display outside the test fixture's 87–99 range.
  D=""
  for n in $(seq 73 86); do
    [ -e "/tmp/.X11-unix/X$n" ] || { D=":$n"; break; }
  done
  [ -n "$D" ] || { echo "no free display number in 73-86" >&2; exit 1; }

  Xvfb "$D" -screen 0 "$SCREEN" -noreset >/dev/null 2>&1 & xvfb_pid=$!
  for _ in $(seq 1 50); do
    if command -v xdpyinfo >/dev/null; then
      xdpyinfo -display "$D" >/dev/null 2>&1 && break
    else
      [ -e "/tmp/.X11-unix/X${D#:}" ] && break
    fi
    sleep 0.1
  done
  kill -0 "$xvfb_pid" 2>/dev/null || { echo "Xvfb $D failed to start" >&2; exit 1; }

  DISPLAY="$D" "$COMPOSITOR" >/dev/null 2>&1 & comp_pid=$!
  sleep 1
  export DISPLAY="$D"
  echo "suite: owned display $D ($SCREEN)  xvfb=$xvfb_pid compositor=$comp_pid"
fi

echo "suite: grab=$GRAB"
echo "suite: artifacts under $OUT_ROOT/"
echo

# ── Play ────────────────────────────────────────────────────────────────────
declare -a RESULTS
FAILED=0
for doc in "${DOCS[@]}"; do
  stem="$(basename "$doc" .json)"
  out="$OUT_ROOT/$stem"
  rm -rf "$out" && mkdir -p "$out"
  start=$SECONDS
  ( cd "$out" && "$GRAB" play "$doc" \
        --trail --feedback --trace \
        --report run.jsonl \
        --log-level debug --log-file grab.log ) >"$out/play.log" 2>&1
  rc=$?
  took=$(( SECONDS - start ))
  if [ "$rc" -eq 0 ]; then
    RESULTS+=("PASS  ${took}s  $stem")
    echo "PASS  ${took}s  $stem"
  else
    FAILED=1
    RESULTS+=("FAIL  ${took}s  $stem (exit $rc)")
    echo "FAIL  ${took}s  $stem (exit $rc) — tail of play.log:"
    tail -n 12 "$out/play.log" | sed 's/^/      /'
  fi
done

echo
echo "── suite summary ──"
printf '%s\n' "${RESULTS[@]}"
exit "$FAILED"
