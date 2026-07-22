#!/usr/bin/env bash
# Smoke for config-driven batch: self-provision Xvfb, capture a helper window,
# validate the manifest, then verify a failed target produces a nonzero exit.
# Usage: config_batch_smoke.sh [build-dir] [helper-executable]
set -euo pipefail

BUILD_DIR="${1:-build}"
GRAB="$BUILD_DIR/grab"
HELPER="${2:-}"
WORK="$(mktemp -d)"
CONFIG="$WORK/batch-profile.json"
FAILED_CONFIG="$WORK/failed-profile.json"
DISPLAY_FILE="$WORK/display-number"
XVFB_LOG="$WORK/xvfb.log"
BATCH_LOG="$WORK/batch.log"
FAILED_LOG="$WORK/failed-batch.log"
POLL_ATTEMPTS=100
POLL_DELAY=0.1
XVFB_PID=""

valid_pid() {
    [[ "$1" =~ ^[0-9]+$ ]] && (( "$1" > 1 ))
}

cleanup() {
    set +e
    if valid_pid "${XVFB_PID:-}"; then
        kill -TERM "$XVFB_PID" 2>/dev/null
        wait "$XVFB_PID" 2>/dev/null
    fi
    rm -rf -- "$WORK"
}
trap cleanup EXIT

command -v Xvfb >/dev/null
command -v python3 >/dev/null
command -v realpath >/dev/null
command -v timeout >/dev/null
if [[ ! -x "$GRAB" ]]; then
    echo "config_batch smoke: grab executable not found at $GRAB" >&2
    exit 1
fi
if [[ -z "$HELPER" ]]; then
    if [[ -x "$BUILD_DIR/tests/grab_config_batch_window" ]]; then
        HELPER="$BUILD_DIR/tests/grab_config_batch_window"
    else
        HELPER="$BUILD_DIR/grab_config_batch_window"
    fi
fi
if [[ ! -x "$HELPER" ]]; then
    echo "config_batch smoke: helper executable not found at $HELPER" >&2
    exit 1
fi
HELPER="$(realpath "$HELPER")"

Xvfb -displayfd 3 -noreset -nolisten tcp -screen 0 640x480x24 \
    3>"$DISPLAY_FILE" >"$XVFB_LOG" 2>&1 &
XVFB_PID=$!
for _ in $(seq 1 "$POLL_ATTEMPTS"); do
    [[ -s "$DISPLAY_FILE" ]] && break
    if ! kill -0 "$XVFB_PID" 2>/dev/null; then
        cat "$XVFB_LOG" >&2
        exit 1
    fi
    sleep "$POLL_DELAY"
done
if [[ ! -s "$DISPLAY_FILE" ]]; then
    echo "config_batch smoke: Xvfb did not publish a display" >&2
    cat "$XVFB_LOG" >&2
    exit 1
fi
read -r DISPLAY_NUMBER <"$DISPLAY_FILE"
if [[ ! "$DISPLAY_NUMBER" =~ ^[0-9]+$ ]]; then
    echo "config_batch smoke: invalid Xvfb display number" >&2
    exit 1
fi
export DISPLAY=":$DISPLAY_NUMBER"

python3 - "$CONFIG" "$HELPER" <<'PY'
import json
import sys

profile = {
    "schema_version": 1,
    "batch": {"output_root": "sessions"},
    "targets": [
        {
            "name": "smoke",
            "argv": [sys.argv[2]],
            "match": "wm_class",
            "pattern": "GrabConfigBatchWindow",
            "frames": 2,
            "interval_ms": 50,
            "delay_ms": 50,
            "timeout_s": 30,
            "kill_after": True,
        }
    ],
}
with open(sys.argv[1], "w", encoding="utf-8") as stream:
    json.dump(profile, stream)
PY

set +e
timeout --foreground 60s "$GRAB" batch --config "$CONFIG" >"$BATCH_LOG" 2>&1
BATCH_STATUS=$?
set -e
if (( BATCH_STATUS != 0 )); then
    echo "config_batch smoke: successful profile exited $BATCH_STATUS" >&2
    cat "$BATCH_LOG" >&2
    exit 1
fi

shopt -s nullglob
SESSIONS=("$WORK"/sessions/*)
if (( ${#SESSIONS[@]} != 1 )); then
    echo "config_batch smoke: expected one session, found ${#SESSIONS[@]}" >&2
    cat "$BATCH_LOG" >&2
    exit 1
fi
SESSION="${SESSIONS[0]}"
python3 - "$SESSION" "$CONFIG" <<'PY'
import json
import os
import sys

session, profile = sys.argv[1:]
manifest_path = os.path.join(session, "manifest.json")
with open(manifest_path, encoding="utf-8") as stream:
    manifest = json.load(stream)
if manifest.get("profile") != profile or manifest.get("state") != "done":
    raise SystemExit(1)
if not manifest.get("started_at") or not manifest.get("ended_at"):
    raise SystemExit(1)
targets = manifest.get("targets")
if not isinstance(targets, list) or len(targets) != 1:
    raise SystemExit(1)
target = targets[0]
if target.get("name") != "smoke" or target.get("error") != "":
    raise SystemExit(1)
if not isinstance(target.get("pid"), int) or target["pid"] <= 1:
    raise SystemExit(1)
if not isinstance(target.get("window_id"), int) or target["window_id"] <= 0:
    raise SystemExit(1)
if target.get("files") != ["smoke.png", "smoke_002.png"]:
    raise SystemExit(1)
for name in target["files"]:
    path = os.path.join(session, "current", name)
    if not os.path.isfile(path) or os.path.getsize(path) <= 0:
        raise SystemExit(1)
PY

python3 - "$FAILED_CONFIG" <<'PY'
import json
import sys

profile = {
    "schema_version": 1,
    "batch": {"output_root": "failed-sessions"},
    "targets": [
        {
            "name": "false-target",
            "argv": ["/bin/false"],
            "match": "pid",
            "timeout_s": 30,
            "kill_after": True,
        }
    ],
}
with open(sys.argv[1], "w", encoding="utf-8") as stream:
    json.dump(profile, stream)
PY

set +e
timeout --foreground 60s "$GRAB" batch --config "$FAILED_CONFIG" \
    >"$FAILED_LOG" 2>&1
FAILED_STATUS=$?
set -e
if (( FAILED_STATUS != 1 )); then
    echo "config_batch smoke: failed profile exited $FAILED_STATUS" >&2
    cat "$FAILED_LOG" >&2
    exit 1
fi

FAILED_SESSIONS=("$WORK"/failed-sessions/*)
if (( ${#FAILED_SESSIONS[@]} != 1 )); then
    echo "config_batch smoke: expected one failed session" >&2
    cat "$FAILED_LOG" >&2
    exit 1
fi
python3 - "${FAILED_SESSIONS[0]}" <<'PY'
import json
import os
import sys

with open(os.path.join(sys.argv[1], "manifest.json"), encoding="utf-8") as stream:
    manifest = json.load(stream)
targets = manifest.get("targets")
if manifest.get("state") != "failed" or not manifest.get("ended_at"):
    raise SystemExit(1)
if not isinstance(targets, list) or len(targets) != 1:
    raise SystemExit(1)
if not targets[0].get("error") or targets[0].get("files") != []:
    raise SystemExit(1)
PY

echo "config_batch smoke OK"
