<p align="center">
  <img src=".art/grab_banner.png" alt="grab" width="760">
</p>

# grab

A single C++ library and `grab` CLI for OS-level desktop automation on X11/Linux —
**observe** input, **capture** output, and **synthesize** input.

- **Observe** — global keyboard/mouse (XInput2), window & focus tracking, AT-SPI, evdev.
- **Capture** — focus-safe window / region / display screenshots (XComposite + XShm),
  an in-tree PNG encoder, and X11 video recording.
- **Synthesize** — mouse move & click, typing, and drag gestures (XTest + xkbcommon).

## Build

```sh
cmake --preset default
cmake --build build -j"$(nproc)"
ctest --test-dir build
```

Requires X11. Toolchain: C++23, Clang, CMake + Ninja.

## Usage

```sh
grab capture --display --out shot.png          # whole display
grab capture --window <WM_CLASS> --out win.png # a single window
grab capture --region X,Y,WxH --out region.png # a region
grab click --at X,Y                            # synthesize a click
grab type  --text "hello"                      # synthesize typing
grab doctor                                    # environment & capability report
```
