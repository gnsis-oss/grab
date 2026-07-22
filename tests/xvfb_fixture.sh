#!/bin/sh
# CTest fixture: provision the Xvfb displays the X-backed test suites use
# (Seat/Locator/Gestures/Input on :94-:96, WindowX11/XInput2 on :95/:97, etc.).
# `start` launches any missing displays (detached, pids recorded) and waits for
# readiness; `stop` kills only the displays this fixture started.
set -eu

DISPLAYS="87 88 89 90 91 92 93 94 95 96 97 98 99"
COMPOSITOR_DISPLAYS="88 98"
PIDFILE="${XVFB_PIDFILE:-/tmp/grab-xvfb.pids}"
COMPOSITOR_PIDFILE="${PIDFILE}.compositors"
SCREEN="-screen 0 1280x1024x24"

start_compositor() {
    d="$1"
    if command -v picom >/dev/null 2>&1; then
        picom_pidfile="${COMPOSITOR_PIDFILE}.${d}.picom"
        rm -f "$picom_pidfile"
        if DISPLAY=":$d" picom --backend xrender -b --config /dev/null \
            --write-pid-path "$picom_pidfile" >/dev/null 2>&1; then
            if [ -s "$picom_pidfile" ]; then
                read -r compositor_pid <"$picom_pidfile"
                [ -n "$compositor_pid" ] && echo "$compositor_pid" >>"$COMPOSITOR_PIDFILE"
            fi
            return
        fi
        rm -f "$picom_pidfile"
    fi
    if command -v xcompmgr >/dev/null 2>&1; then
        DISPLAY=":$d" xcompmgr >/dev/null 2>&1 &
        echo "$!" >>"$COMPOSITOR_PIDFILE"
    fi
}

stop_compositor() {
    if [ -f "$COMPOSITOR_PIDFILE" ]; then
        while IFS= read -r pid; do
            [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
        done <"$COMPOSITOR_PIDFILE"
        rm -f "$COMPOSITOR_PIDFILE"
    fi
    for d in $COMPOSITOR_DISPLAYS; do
        rm -f "${COMPOSITOR_PIDFILE}.${d}.picom"
    done
}

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
        : >"$COMPOSITOR_PIDFILE"
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
        for d in $COMPOSITOR_DISPLAYS; do
            start_compositor "$d"
        done
        ;;
    stop)
        stop_compositor
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
