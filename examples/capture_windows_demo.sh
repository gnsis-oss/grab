#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# grab visual example: spawn graphic windows on an isolated virtual X display,
# then screenshot them to PNG using grab's OWN XCB/XShm/XComposite capture
# pipeline + in-tree PNG encoder. No GNOME screenshot tools are involved.
#
#   Usage:  examples/capture_windows_demo.sh [OUTPUT_DIR]
#   Default OUTPUT_DIR: examples/screenshots/
# ---------------------------------------------------------------------------
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"
grab="$repo/build/grab"
out_dir="${1:-$here/screenshots}"
display=":95"                       # isolated; avoid the test fixtures (:87-:89)
geom="1024x768x24"

command -v Xvfb   >/dev/null || { echo "need Xvfb";   exit 1; }
command -v xeyes  >/dev/null || { echo "need x11-apps (xeyes)"; exit 1; }
[ -x "$grab" ] || { echo "build grab first: cmake --build build -j"; exit 1; }

mkdir -p "$out_dir"
pids=()
cleanup() {
    for p in "${pids[@]:-}"; do kill "$p" 2>/dev/null || true; done
    [ -n "${xvfb_pid:-}" ] && kill "$xvfb_pid" 2>/dev/null || true
}
trap cleanup EXIT

# 1. Start an isolated virtual X server (NOT a GNOME/desktop session).
echo "[1/5] starting Xvfb on $display ($geom)"
Xvfb "$display" -screen 0 "$geom" -nolisten tcp >/dev/null 2>&1 &
xvfb_pid=$!
export DISPLAY="$display"
for _ in $(seq 1 50); do
    xdpyinfo >/dev/null 2>&1 && break || sleep 0.1
done

# 2. Paint a background so the capture is visibly not-black, then spawn windows.
echo "[2/5] spawning graphic windows (xsetroot, xeyes, xclock, xlogo, xmessage)"
xsetroot -solid rgb:20/40/60 2>/dev/null || xsetroot -solid steelblue 2>/dev/null || true
# xeyes lives at (80,80) size 220x160 -- reused below for the region capture.
xeyes_x=80 xeyes_y=80 xeyes_w=220 xeyes_h=160
xeyes   -geometry ${xeyes_w}x${xeyes_h}+${xeyes_x}+${xeyes_y}   & pids+=($!)
xclock  -geometry 200x200+360+80  -update 1                     & pids+=($!)
xlogo   -geometry 200x200+600+80                                & pids+=($!)
xmessage -geometry 380x120+180+340 -default okay \
         "grab captured this window on $display" -timeout 60     & pids+=($!)

# 3. Give the apps a moment to map and draw.
echo "[3/5] waiting for windows to render"
sleep 2

# 4. Capture the WHOLE virtual display with grab (its XShm root-region path).
echo "[4/5] grab capture --display  ->  full_display.png"
"$grab" capture --display --out "$out_dir/full_display.png"

# 5. Capture a SINGLE window's area with grab's region path (XShm sub-region).
#    grab's --window matcher needs a window manager (it reads the EWMH
#    _NET_CLIENT_LIST); a bare Xvfb has none, so we capture xeyes by its known
#    geometry instead -- same grab capture pipeline, no WM required.
echo "[5/5] grab capture --region ${xeyes_x},${xeyes_y},${xeyes_w}x${xeyes_h}  ->  window_xeyes.png"
"$grab" capture --region "${xeyes_x},${xeyes_y},${xeyes_w}x${xeyes_h}" \
        --out "$out_dir/window_xeyes.png"

echo
echo "Saved PNG screenshots to: $out_dir"
for f in "$out_dir"/full_display.png "$out_dir"/window_xeyes.png; do
    [ -e "$f" ] && printf '  %-20s %8s bytes  %s\n' \
        "$(basename "$f")" "$(stat -c%s "$f")" "$(file -b "$f")"
done

# Self-check: prove the capture is real content, not a blank frame.
echo
echo "Content check (no GNOME tools used anywhere):"
python3 - "$out_dir/full_display.png" <<'PY'
import struct, zlib, sys
raw=open(sys.argv[1],'rb').read(); i=8; idat=b''; w=h=ct=0
while i<len(raw):
    ln=struct.unpack('>I',raw[i:i+4])[0]; t=raw[i+4:i+8]; c=raw[i+8:i+8+ln]
    if t==b'IHDR': w,h,_,ct=struct.unpack('>IIBB',c[:10])
    if t==b'IDAT': idat+=c
    i+=12+ln
d=zlib.decompress(idat); ch=4 if ct==6 else 3; stride=w*ch+1
cols={d[y*stride+1+x*ch : y*stride+1+x*ch+ch] for y in range(0,h,4) for x in range(0,w,4)}
print(f"  full_display.png: {w}x{h}, {len(cols)} distinct colors -> "
      + ("real screenshot with window content" if len(cols)>10 else "looks blank"))
PY
