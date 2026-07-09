#!/bin/sh
# CTest fixture: provision the Xvfb displays the X-backed test suites use
# (Seat/Locator/Gestures/Input on :94-:96, WindowX11/XInput2 on :95/:97, etc.).
# `start` launches any missing displays (detached, pids recorded) and waits for
# readiness; `stop` kills only the displays this fixture started.
set -eu

DISPLAYS="87 88 89 90 91 92 93 94 95 96 97 98 99"
PIDFILE="${XVFB_PIDFILE:-/tmp/grab-xvfb.pids}"
SCREEN="-screen 0 1280x1024x24"

wait_ready() {
    d="$1"
    i=0
    until xdpyinfo -display ":$d" >/dev/null 2>&1; do
        i=$((i + 1))
        [ "$i" -gt 100 ] && return 1
        sleep 0.1
    done
    return 0
}

case "${1:-}" in
    start)
        : >"$PIDFILE"
        for d in $DISPLAYS; do
            if ! xdpyinfo -display ":$d" >/dev/null 2>&1; then
                # shellcheck disable=SC2086
                nohup Xvfb ":$d" $SCREEN >/dev/null 2>&1 &
                echo "$!" >>"$PIDFILE"
            fi
        done
        for d in $DISPLAYS; do
            wait_ready "$d" || { echo "Xvfb :$d did not become ready" >&2; exit 1; }
        done
        ;;
    stop)
        if [ -f "$PIDFILE" ]; then
            while IFS= read -r pid; do
                [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
            done <"$PIDFILE"
            rm -f "$PIDFILE"
        fi
        ;;
    *)
        echo "usage: $0 {start|stop}" >&2
        exit 2
        ;;
esac
