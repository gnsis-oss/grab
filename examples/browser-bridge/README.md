# grab browser bridge

Wires a real browser's tab events to the grab browser examples
(`event_logger`). The example opens a unix socket; this kit connects the
browser to it via a native-messaging host.

```
browser + extension  --native messaging-->  grab-browser-host (socat)
                                                     |
                                              unix socket
                                                     |
                                          example's BrowserSocket -> BrowserBridge
```

Contents:
- `extension/` — an unpacked MV3 extension that sends **flat** `app.tab_changed`
  frames (the grab bridge rejects nested JSON).
- `grab-browser-host` — the native-messaging host; forwards stdio to the socket
  with `socat` (install `socat` first).
- `com.eventgrab.bridge.json` — native-messaging host manifest template.

## Prerequisites

- `socat` on PATH (`apt install socat`).
- Run the example first so the socket exists, e.g.
  `./build/examples/event_logger` — it prints `browser socket at <path>`.
  That path must match `grab-browser-host` (default
  `$XDG_RUNTIME_DIR/grab-event-logger.sock`; override with
  `GRAB_BRIDGE_SOCKET`).

## Install (Chrome / Chromium)

1. Make the host executable and note its absolute path:
   `chmod +x examples/browser-bridge/grab-browser-host`
2. Load the extension: `chrome://extensions` → enable **Developer mode** →
   **Load unpacked** → select `examples/browser-bridge/extension`. Copy the
   **extension ID** it shows.
3. Fill in the host manifest: edit `com.eventgrab.bridge.json` — set `path` to
   the absolute path of `grab-browser-host`, and replace
   `REPLACE_WITH_EXTENSION_ID` in `allowed_origins` with the ID from step 2.
4. Install the manifest (filename must equal the host name):
   `cp com.eventgrab.bridge.json ~/.config/google-chrome/NativeMessagingHosts/`
   (Chromium: `~/.config/chromium/NativeMessagingHosts/`).
5. Reload the extension. Switch tabs — `app.tab_changed` events appear in the
   example's live feed and JSONL output.

## Install (Firefox)

1–3 as above, but you do **not** need the extension ID: the extension declares
a fixed id (`grab-bridge@example.com`) already listed in the manifest's
`allowed_extensions`. Just set `path`.
4. `cp com.eventgrab.bridge.json ~/.mozilla/native-messaging-hosts/`
5. Load the extension: `about:debugging` → **This Firefox** →
   **Load Temporary Add-on** → pick `extension/manifest.json`. Switch tabs.

## Verify without a browser

Confirm the socket/capture pipeline independently by sending one frame:

```bash
python3 - "$XDG_RUNTIME_DIR/grab-event-logger.sock" <<'EOF'
import socket, struct, sys
f = b'{"type":"app.tab_changed","app":"test","title":"Hello","url":"http://x"}'
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1]); s.sendall(struct.pack('<I', len(f)) + f); s.close()
EOF
```

The event should appear in the feed immediately. If this works but the browser
doesn't, the issue is the extension/manifest wiring, not grab.

## Troubleshooting

- **No connection:** the example must be running (socket must exist) *before*
  the browser launches the host; the extension retries every 5s.
- **Host not found:** the manifest filename must be exactly
  `com.eventgrab.bridge.json` and its `name` must match `HOST_NAME` in
  `background.js`.
- **Frames rejected:** the grab bridge accepts only flat JSON — keep frames
  scalar-only (this extension already does).
