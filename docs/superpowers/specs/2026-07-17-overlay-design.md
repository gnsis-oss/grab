# Spec: overlay — click-through annotation surface (scene, rasterizer, trail)

- **Date:** 2026-07-17
- **Status:** approved design (brainstorm with user); implementation plan to follow
- **Relation to the canonical architecture:** an *addition* under the
  2026-07-13 canonical spec's rules — new named kernel concern, new SPI
  surface, new capability row, one public header (allowlist addition). It
  changes no existing contract. The 18 invariants apply as review gates;
  the ones that bind hardest here are #3 (space-tagged coordinates), #14
  (no raw sleeps — pacing via typed governor), and #16 (closed, table-driven
  vocabularies).

## 1. Purpose

A debug/annotation subsystem: draw transparent, always-on-top, click-through
graphics over the desktop — shapes, paths, and a mouse trail — so a human (or
an agent, later, over the wire) can *see* what grab observes and does: where a
locator resolved, what a synthesized drag actually did, whether motion was
physical or injected.

**Design law:** the kernel owns everything about drawing; platform providers
only blit. The platform contract is a rasterized ARGB frame + damage rects.
This is what makes the same debug code portable across X11, Wayland
compositors, win32, and macOS: the per-platform shim is ~100 lines of
"present a buffer", and every drawing feature added later costs the backends
nothing.

## 2. Non-goals (this spec)

- **Text rendering** — deferred. Fonts drag in freetype/fontconfig decisions
  the dependency policy must rule on separately; a bitmap-label fallback is
  the likely later route. The primitive vocabulary is closed but extensible
  by design, so text arrives as a new table row, not a redesign.
- **Wire exposure** (remote clients drawing over the daemon socket) — Phase 2.
  The in-process public API lands first; the RPC is mechanical once
  `CommandDescriptor` rows exist for the scene verbs.
- **Input on the overlay** — never. The surface is click-through always;
  it must be impossible for an overlay to steal or occlude input. (An overlay
  that can absorb a click is a safety hazard, not a feature.)
- **Non-X11 providers** — specified by mechanism (§7) so the seam is proven,
  implemented later (GNOME-Wayland bridge lands with the WP10 Wayland phase).

## 3. Kernel: `src/kernel/presentation/`

Three platform-free components, unit-testable without any display.

### 3.1 `overlay_scene.{hpp,cpp}` — retained scene

- Verbs: `add(Shape) -> ShapeId`, `update(ShapeId, Shape)`, `remove(ShapeId)`,
  `clear()`. `ShapeId` is generation-scoped like every other grab id.
- Z-order: insertion order within two bands (`Band::Annotation` below
  `Band::Trail`), explicit `z` override per shape.
- Lifetime policy per shape (closed enum): `Persistent`, `Ttl{ms}`,
  `Fade{ms}` (alpha ramps to zero, then auto-remove).
- Damage: every mutation and every fade tick yields minimal damage rects;
  the scene never forces full-frame redraws in steady state.

### 3.2 Primitive vocabulary (closed, table-driven)

| Shape | Geometry | Notes |
|---|---|---|
| `Path` | polyline points and/or beziers (`geometry/curve.hpp` forms) | open or closed |
| `Rect` | `geometry/rectangle.hpp` | |
| `Ellipse` | center + radii | circle = equal radii |
| `Polygon` | point list, closed | |

Style on every shape: stroke `{color RGBA, width px}`, optional fill
`{color RGBA}`. Alpha is part of color; the surface is premultiplied ARGB.

**Coordinates:** every point/rect in a shape carries its `CoordinateSpaceId`.
The scene transforms into the surface's space through the space graph at
rasterization time. Untagged coordinates do not exist in the API (invariant
#3); "draw this Match's bounds" is provenance-correct by construction.

### 3.3 `overlay_raster.{hpp,cpp}` — rasterizer

In-tree anti-aliased scanline rasterizer producing the premultiplied-ARGB
`grab::Image` frame + damage rects. No new dependencies (dependency policy:
the PNG codec precedent — we own pixel code). Golden-frame tested (§9).

### 3.4 `trail_animator.{hpp,cpp}` — the trail as a scene client

Consumes origin-stamped pointer-motion events from the bus subscription
surface; appends fading `Path` segments into the `Trail` band:
red = `EventOrigin::Physical`, blue = `EventOrigin::InjectedSelf`
(defaults; CLI-overridable). Default fade 1200 ms, width 3 px. Motion
coalescing on the bus under backpressure is acceptable and intentionally
visible — the trail shows what subscribers actually see.

### 3.5 Pacing

Frame tick (target 60 Hz, damage-gated: skip when no damage) rides the
existing `PacingGovernor` + reactor timer. No raw sleeps (invariant #14).

## 4. SPI: `src/spi/overlay_surface.hpp`

```cpp
class OverlaySurface
{
  public:
    virtual Result<void> open( geometry::Size size, CoordinateSpaceId space ) = 0;
    virtual Result<void> present( const Image& argb_premultiplied,
                                  std::span<const geometry::Rectangle> damage ) = 0;
    virtual void         close() = 0;
};
```

- `open` declares the surface's coordinate space (its transform is registered
  in the space graph by the providing runtime).
- `present` blits only the damage; the full frame is available for providers
  that cannot partial-blit.
- The surface is click-through and top-most by contract; a provider that
  cannot guarantee both must not register the capability.
- `Runtime` gains `virtual OverlaySurface* overlay_surface()` (nullptr when
  unsupported), symmetric with `InputSeat*`/`TreeSource*`.

## 5. Capability + resolution

New row `Capability::Overlay` in the existing enum/table. Resolution goes
through the provider registry like capture/input; failure surfaces the
existing `ErrorCode::CapabilityUnavailable` with resolver reasons (doctor
shows it like any other capability).

## 6. Public API + CLI

- New public header `include/grab/overlay.hpp` (allowlist addition):
  `session.overlay() -> Result<Overlay&>` exposing the scene verbs and shape
  types. Shape geometry uses the existing public `geometry/*` and space
  vocabulary. Nothing in the header names a platform.
- CLI (`src/frontends/cli/overlay_command.cpp`):
  - `grab overlay trail [--color --injected-color --fade-ms --width]` — runs
    the animator until interrupted. `grab trail` is an alias.
  - `grab overlay rect|ellipse|path --at ... [--ttl ms | --fade ms | --hold]`
    — one-shot annotations (`--hold` keeps the process alive holding
    persistent shapes until Ctrl-C).
- Frontends compose public services only; no driver includes (dependency
  rule, CI-checked).

## 7. Providers (folder abstraction)

| Path | Mechanism | Status |
|---|---|---|
| `src/drivers/desktop/x11/overlay_surface.cpp` | ARGB override-redirect window on the composited screen; `_NET_WM_STATE_ABOVE`; XFixes empty *input* region (click-through); XShm blit; on unredirected/fullscreen contexts falls back to full-frame present | **Phase 1 (now)** |
| `src/drivers/desktop/wayland/layer_shell_surface.cpp` | `zwlr_layer_shell_v1` overlay layer + `wl_shm`, empty input region | later (KDE/wlroots) |
| `src/drivers/desktop/wayland/gnome_shell_bridge.cpp` | thin JS GNOME Shell extension drawing on the Clutter stage, fed damage-limited frames over the daemon socket; the extension owns zero logic | later (with WP10) |
| `src/drivers/desktop/win32/layered_overlay.cpp` | `WS_EX_LAYERED\|WS_EX_TRANSPARENT\|WS_EX_TOPMOST` + `UpdateLayeredWindow` | later |
| `src/drivers/desktop/macos/overlay_window.cpp` | borderless `NSWindow` at overlay window level, `ignoresMouseEvents` | later |

The X11 provider must handle: RandR geometry changes (reopen at new size,
space graph updated), compositor absence (no ARGB visual → typed
`CapabilityUnavailable`, never a black rectangle). Captures include the
overlay by default — seeing the annotation in the screenshot is usually the
point. An opt-out (`capture --without-overlay`, implemented as hide →
capture → show around the capture critical section via a `set_visible()`
on the surface) is Phase 2.

## 8. Error handling

Existing taxonomy only: `CapabilityUnavailable` (no provider / no compositor),
`DisplayUnavailable`, plus typed staleness on `ShapeId` reuse after `clear()`
(generation mismatch). No new error codes anticipated; if implementation
finds a gap, the code is added to the descriptor table with disposition data
per invariant #15.

## 9. Testing

1. **Scene unit tests** — id/generation semantics, lifetime policies, damage
   minimality (mutating one shape damages only its bounds union).
2. **Rasterizer golden frames** — scripted scenes rendered and compared
   against committed PNGs (decoded by our own codec); antialiasing tolerance
   band documented in the test.
3. **Trail animator** — scripted motion event sequences (both origins) →
   expected segment set; coalescing-under-backpressure behavior pinned.
4. **Ring-3 self-verifying (Xvfb)** — the flagship test: open a session,
   draw a rect via the public API, run the trail, synthesize a drag through
   grab's own input path, capture through grab's own capture path, assert
   rect pixels and blue trail pixels along the drag line. Overlay, input,
   and capture dogfooded in one test.
5. **Invariant CI** — `overlay.hpp` added to the public-header allowlist in
   the same commit that creates it; kernel-platform-free rule already covers
   `src/kernel/presentation/`.

## 10. Phasing

- **Phase 1 (now):** kernel scene/raster/animator + SPI + `Capability::Overlay`
  + X11 provider + public `overlay.hpp` + CLI verbs + tests above.
- **Phase 2:** wire exposure — `CommandDescriptor` rows for scene verbs,
  loopback + gRPC dispatch, per-client shape namespaces (a client's shapes
  die with its connection), `ControlGrant` gating once the safety-policy work
  lands.
- **Phase 3:** non-X11 providers per §7, ordered by need (GNOME-Shell bridge
  first, with WP10).
