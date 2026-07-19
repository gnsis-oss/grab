#!/usr/bin/env bash
# Smoke for examples/browser_event_screenshot: Xvfb + a canned native-messaging
# browser frame piped into the socket; asserts a timestamped PNG lands in the
# output dir. No real browser needed. Usage: browser_event_screenshot_smoke.sh [build-dir]
set -euo pipefail

BUILD_DIR="${1:-build}"
DISPLAY_NUM=":97"
WORK="$(mktemp -d)"
SOCK="$WORK/browser.sock"
SHOTS="$WORK/shots"
FEED="$WORK/out.txt"
cleanup() {
    kill "${LOGGER_PID:-0}" "${XVFB_PID:-0}" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

Xvfb "$DISPLAY_NUM" -noreset -screen 0 1280x800x24 &
XVFB_PID=$!
sleep 1

DISPLAY="$DISPLAY_NUM" "$BUILD_DIR/examples/browser_event_screenshot" "$SHOTS" --socket "$SOCK" > "$FEED" &
LOGGER_PID=$!
sleep 1

python3 - "$SOCK" <<'EOF'
import socket, struct, sys
frames = [
    b'{"type":"browser.tab_switched","tab_title":"SmokeTab","app":"chrome","pid":"1"}',
    b'{"type":"browser.context","url":"https://example.com","app":"chrome","pid":"1"}',
]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
for f in frames:
    s.sendall(struct.pack('<I', len(f)) + f)
s.close()
EOF
sleep 2

kill -INT "$LOGGER_PID"
wait "$LOGGER_PID"

# At least one screenshot exists and is a valid PNG (magic bytes).
shopt -s nullglob
pngs=("$SHOTS"/*.png)
if [ "${#pngs[@]}" -eq 0 ]; then
    echo "browser_event_screenshot smoke FAILED: no PNG produced"
    cat "$FEED"
    exit 1
fi
for png in "${pngs[@]}"; do
    magic=$(head -c 4 "$png" | od -An -tx1 | tr -d ' \n')
    if [ "$magic" != "89504e47" ]; then
        echo "browser_event_screenshot smoke FAILED: $png is not a PNG ($magic)"
        exit 1
    fi
done
grep -q 'saved .*tab_changed.png (tab "SmokeTab")' "$FEED"
echo "browser_event_screenshot smoke OK (${#pngs[@]} screenshots)"
