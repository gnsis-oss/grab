<p align="center">
  <img src=".art/grab_banner.png" alt="grab" width="760">
</p>


Grab is a library and CLI tool for interacting with a PC

Its very useful if you want to :

* Automate clicks for App navigation
* Control your mouse via coordinates and paths 
* Harness AI to grab screenshots of your project
* Visual Testing Automation


## Support

### Operating systems

| OS               | Status        | Backend                        |
| ---------------- | ------------- | ------------------------------ |
| Linux · X11      | ✅ Supported  | XCB / XInput2 / XTest          |
| Linux · Wayland  | 🚧 Planned    | —                              |
| Windows          | 🚧 Planned    | —                              |
| macOS            | 🚧 Planned    | —                              |

### Technologies

| Capability          | Technology                          |
| ------------------- | ----------------------------------- |
| Screen capture      | XComposite + XShm                   |
| Screenshots (PNG)   | in-tree encoder + zlib              |
| Video recording     | libavcodec (FFmpeg)                 |
| Input synthesis     | XTest + xkbcommon                   |
| Input observation   | XInput2, evdev                      |
| Windows & focus     | XCB / EWMH                          |
| Accessibility       | AT-SPI over D-Bus                   |
| Event daemon        | gRPC + Protobuf                     |
| Notifications       | D-Bus                               |
| Overlay annotations | XFixes/XRender ARGB + compositor    |



## Provisioning a display

grab can drive an application on a display — and it can give you a display it
can drive. `provision_display()` starts one and satisfies the preconditions
grab itself imposes, so a consumer no longer hand-rolls them:

```cpp
#include <grab/provisioning.hpp>

auto display = grab::provision_display( { .backend = grab::DisplayBackend::Headless } );
if( !display ) { /* the reason names what to install */ }

for( const auto& [key, value] : display->child_environment() )
{
    // launch your own application with these set: DISPLAY, the session bus,
    // and the AT-SPI bridge switches
}

auto session = grab::open_session( *display );
```

Two of those preconditions fail *silently* when they are missing, which is why
they are grab's business rather than the caller's:

| Precondition | Started as | What its absence looks like |
| --- | --- | --- |
| X display | Xvfb, or Xephyr for `Nested` | nothing to drive |
| window manager | first installed of openbox, fluxbox, … | `Input::click` succeeds; the link never activates |
| compositing manager | first installed of picom, xcompmgr, … | `Session::overlay()` draws no pixels |
| session bus | `dbus-daemon --session` | the accessibility bus has nowhere to sit |
| accessibility bus | `at-spi-bus-launcher` | `resolve`/`describe` see no tree |

`window_manager()`, `compositor()` and `accessibility()` report which are live,
each failure carrying the usual `CapabilityUnavailable` reason. They are probed
on every call, so the same query answers for a display grab started
(`Headless`, `Nested`) and for one it attached to (`Existing`, which spawns
nothing — a second window manager on a desktop somebody is using would be
destructive). Destruction tears down exactly what was started, by recorded
pidfd, SIGTERM before SIGKILL, never by process name.

```sh
build/dev/examples/provision_display          # headless, then print the report
```

## Build

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Requires X11. Toolchain: C++23, Clang, CMake + Ninja. Process ownership via
`grab::OwnedProcess` requires Linux 5.4 or newer and glibc 2.36 or newer.

**X11 platform floor: XInput 2.1.** Under XI 2.0 the server does not deliver
raw events to non-grabbing clients while a pointer grab is active, so anything
observing input during a drag goes blind for the duration. grab checks the
minor version and refuses a server below 2.1. XI 2.1 dates from 2010.

### Build configurations

Instrumentation is an explicit choice, not a consequence of the build type.
Each preset owns its own directory under `build/`, so configuring one never
disturbs another.

| Preset | Directory | Flags | Instrumentation | Log ceiling |
| --- | --- | --- | --- | --- |
| `dev` (`default`) | `build/dev` | `-O2 -g` | — (formats + tidies) | debug |
| `debug` | `build/debug` | `-O0 -g3` | — | debug |
| `asan` | `build/asan` | `-O1 -g3` | ASan + UBSan | debug |
| `tsan` | `build/tsan` | `-O1 -g3` | TSan + UBSan | debug |
| `msan` | `build/msan` | `-O1 -g3` | MSan + UBSan | debug |
| `coverage` | `build/coverage` | `-O0 -g` | gcov arcs | nominal |
| `profile` | `build/profile` | `-O2 -g` + frame pointers | perf targets | nominal |
| `release` | `build/release` | `-O3 -DNDEBUG` | — | off |
| `gcc` | `build/gcc` | `-O2 -g`, g++ | — | debug |
| `iwyu` | `build/iwyu` | `-O2 -g` | include-what-you-use | debug |

The options compose freely if none of the presets fit —
`-DGRAB_SANITIZER=none|address|thread|memory`, `-DGRAB_COVERAGE=ON|OFF`,
`-DGRAB_FRAME_POINTERS=ON|OFF`, `-DGRAB_FORMAT=ON|OFF`, `-DGRAB_TIDY=ON|OFF`,
`-DGRAB_LOG_LEVEL=off|nominal|verbose|debug`.

Two things worth knowing:

- **Coverage lives only in the `coverage` preset.** gcov arc counters dominate
  per-pixel loops — the overlay raster goes from ~1 ms to over 100 ms per
  3200x2000 frame — which no interactive build can absorb.
- **`ctest` presets run serially.** A rotating set of display-backed tests
  fails under `-j4` and passes under `-j1`; always confirm a display-backed
  failure serially before believing it.

### Logging

The compile-time ceiling above decides what exists in the binary. What is
actually emitted is a separate runtime level, **off by default**:

```sh
grab trail --log-level verbose --log-tags frame,present
GRAB_LOG=debug GRAB_LOG_FILE=/tmp/grab.log grab sketch
```
