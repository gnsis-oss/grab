#!/usr/bin/env bash
# Smoke for examples/event_logger: Xvfb + xmessage (os events), xdotool
# (input events, skipped if absent), a canned native-messaging frame
# (browser events), and JSONL assertions. Usage: event_logger_smoke.sh [build-dir]
set -euo pipefail

BUILD_DIR="${1:-build}"
DISPLAY_NUM=":97"
WORK="$(mktemp -d)"
SOCK="$WORK/browser.sock"
FEED="$WORK/feed.txt"
cleanup() {
    kill "${LOGGER_PID:-0}" "${XVFB_PID:-0}" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

Xvfb "$DISPLAY_NUM" -noreset -screen 0 1280x800x24 &
XVFB_PID=$!
sleep 1

DISPLAY="$DISPLAY_NUM" "$BUILD_DIR/examples/event_logger" "$WORK/events" --socket "$SOCK" --mouse > "$FEED" &
LOGGER_PID=$!
sleep 1

DISPLAY="$DISPLAY_NUM" xmessage -timeout 2 smoke &
sleep 1
if command -v xdotool >/dev/null; then
    DISPLAY="$DISPLAY_NUM" xdotool key a click 1
    # Scroll detents arrive as buttons 4/5; the feed coalesces them.
    DISPLAY="$DISPLAY_NUM" xdotool click 4 click 4 click 4
    # Relative motion: absolute mousemove emits no XI2 raw events on Xvfb.
    for _ in $(seq 1 30); do
        DISPLAY="$DISPLAY_NUM" xdotool mousemove_relative -- 5 3
        sleep 0.02
    done
else
    echo "smoke: xdotool absent, skipping input assertions"
fi

python3 - "$SOCK" <<'EOF'
import socket, struct, sys
frame = b'{"type":"browser.tab_switched","tab_title":"SmokeTab","app":"chrome","pid":"1"}'
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1]); s.sendall(struct.pack('<I', len(frame)) + frame); s.close()
EOF
sleep 2

kill -INT "$LOGGER_PID"
wait "$LOGGER_PID"

grep -q 'os      event' "$FEED"
grep -q 'browser event -> tab "SmokeTab" focused' "$FEED"
if command -v xdotool >/dev/null; then
    grep -q 'input   event -> key' "$FEED"
    grep -q 'pointer moved' "$FEED"
    grep -q 'scrolled up' "$FEED"
    ! grep -q 'button #4 clicked' "$FEED"
fi
ls "$WORK"/events/*.jsonl >/dev/null
echo "event_logger smoke OK"
