#!/usr/bin/env bash
# Smoke for config-driven watch: a self-provisioned Xvfb, foreground capture,
# then daemon start/status/stop. Usage: config_watch_smoke.sh [build-dir]
set -euo pipefail

BUILD_DIR="${1:-build}"
GRAB="$BUILD_DIR/grab"
WORK="$(mktemp -d)"
RUNTIME_DIR="$WORK/runtime"
FOREGROUND_OUTPUT="$WORK/foreground"
DAEMON_OUTPUT="$WORK/daemon"
CONFIG="$WORK/watch.json"
DISPLAY_FILE="$WORK/display-number"
XVFB_LOG="$WORK/xvfb.log"
FOREGROUND_LOG="$WORK/foreground.log"
STATUS_JSON="$WORK/status.json"
MINIMUM_CAPTURES=3
POLL_ATTEMPTS=100
POLL_DELAY=0.1
FOREGROUND_PID=""
DAEMON_PID=""
XVFB_PID=""

valid_pid() {
    [[ "$1" =~ ^[0-9]+$ ]] && (( "$1" > 1 ))
}

cleanup() {
    set +e
    local cleanup_daemon_pid="${DAEMON_PID:-}"
    if ! valid_pid "$cleanup_daemon_pid" && \
        [[ -r "$RUNTIME_DIR/grab/watch.pid" ]]; then
        read -r cleanup_daemon_pid <"$RUNTIME_DIR/grab/watch.pid"
    fi
    if valid_pid "${FOREGROUND_PID:-}"; then
        kill -TERM "$FOREGROUND_PID" 2>/dev/null
        wait "$FOREGROUND_PID" 2>/dev/null
    fi
    if valid_pid "$cleanup_daemon_pid" && \
        [[ -d "/proc/$cleanup_daemon_pid" ]]; then
        timeout --foreground 10s "$GRAB" watch stop >/dev/null 2>&1
        if [[ -d "/proc/$cleanup_daemon_pid" ]]; then
            kill -TERM "$cleanup_daemon_pid" 2>/dev/null
        fi
        for _ in $(seq 1 20); do
            [[ ! -d "/proc/$cleanup_daemon_pid" ]] && break
            sleep 0.05
        done
        if [[ -d "/proc/$cleanup_daemon_pid" ]]; then
            kill -KILL "$cleanup_daemon_pid" 2>/dev/null
        fi
    fi
    if valid_pid "${XVFB_PID:-}"; then
        kill -TERM "$XVFB_PID" 2>/dev/null
        wait "$XVFB_PID" 2>/dev/null
    fi
    rm -rf -- "$WORK"
}
trap cleanup EXIT

command -v Xvfb >/dev/null
command -v python3 >/dev/null
command -v timeout >/dev/null
if [[ ! -x "$GRAB" ]]; then
    echo "config_watch smoke: grab executable not found at $GRAB" >&2
    exit 1
fi

mkdir -p "$RUNTIME_DIR" "$FOREGROUND_OUTPUT" "$DAEMON_OUTPUT"
chmod 700 "$RUNTIME_DIR"
export XDG_RUNTIME_DIR="$RUNTIME_DIR"

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
    echo "config_watch smoke: Xvfb did not publish a display" >&2
    cat "$XVFB_LOG" >&2
    exit 1
fi
read -r DISPLAY_NUMBER <"$DISPLAY_FILE"
if [[ ! "$DISPLAY_NUMBER" =~ ^[0-9]+$ ]]; then
    echo "config_watch smoke: invalid Xvfb display number" >&2
    exit 1
fi
export DISPLAY=":$DISPLAY_NUMBER"

cat >"$CONFIG" <<'JSON'
{
  "schema_version": 1,
  "watch": {
    "interval_ms": 1000,
    "output": "unused",
    "filename": "capture_{seq}",
    "format": "png"
  }
}
JSON

"$GRAB" watch start "$CONFIG" --interval 100 --output "$FOREGROUND_OUTPUT" \
    >"$FOREGROUND_LOG" 2>&1 &
FOREGROUND_PID=$!
shopt -s nullglob
capture_count=0
for _ in $(seq 1 "$POLL_ATTEMPTS"); do
    captures=("$FOREGROUND_OUTPUT"/*.png)
    capture_count=${#captures[@]}
    if (( capture_count >= MINIMUM_CAPTURES )); then
        break
    fi
    if ! kill -0 "$FOREGROUND_PID" 2>/dev/null; then
        wait "$FOREGROUND_PID" || true
        cat "$FOREGROUND_LOG" >&2
        exit 1
    fi
    sleep "$POLL_DELAY"
done
if (( capture_count < MINIMUM_CAPTURES )); then
    echo "config_watch smoke: foreground captured only $capture_count files" >&2
    cat "$FOREGROUND_LOG" >&2
    exit 1
fi
kill -TERM "$FOREGROUND_PID"
if ! wait "$FOREGROUND_PID"; then
    cat "$FOREGROUND_LOG" >&2
    exit 1
fi
FOREGROUND_PID=""

"$GRAB" watch start "$CONFIG" --daemon --interval 100 --output "$DAEMON_OUTPUT"
status_ready=false
for _ in $(seq 1 "$POLL_ATTEMPTS"); do
    if "$GRAB" watch status --json >"$STATUS_JSON" 2>/dev/null; then
        candidate_pid="$(python3 - "$STATUS_JSON" "$CONFIG" <<'PY'
import json
import os
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    status = json.load(stream)
pid = status.get("pid")
configs = status.get("configs")
if not isinstance(pid, int) or pid <= 1 or status.get("live") is not True:
    raise SystemExit(1)
if not os.path.isdir(f"/proc/{pid}"):
    raise SystemExit(1)
if not isinstance(configs, list):
    raise SystemExit(1)
if not any(item.get("config") == sys.argv[2] for item in configs):
    raise SystemExit(1)
print(pid)
PY
)" || candidate_pid=""
        if valid_pid "$candidate_pid"; then
            DAEMON_PID="$candidate_pid"
            status_ready=true
            break
        fi
    fi
    sleep "$POLL_DELAY"
done
if [[ "$status_ready" != true ]]; then
    echo "config_watch smoke: daemon status never became ready" >&2
    [[ -f "$RUNTIME_DIR/grab/watch.log" ]] && \
        cat "$RUNTIME_DIR/grab/watch.log" >&2
    exit 1
fi

timeout --foreground 10s "$GRAB" watch stop
for _ in $(seq 1 20); do
    [[ ! -d "/proc/$DAEMON_PID" ]] && break
    sleep 0.05
done
if [[ -d "/proc/$DAEMON_PID" ]]; then
    echo "config_watch smoke: daemon pid $DAEMON_PID is still live" >&2
    exit 1
fi
DAEMON_PID=""

echo "config_watch smoke OK"
