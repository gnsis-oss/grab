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

**Design law:** the kernel owns the *vocabulary and the scene truth* —
abstract, space-tagged shape records with declarative lifetime/animation
policies. **Rendering and time belong to the delegate**: the per-platform
provider receives scene deltas and draws them with whatever is native there
(an ARGB X11 window today; the GNOME Shell stage, Direct2D, or CoreAnimation
elsewhere), animating fades on its own frame clock. grab never runs a render
loop and never ships pixels across a boundary; the same debug code is the
same *scene*, not the same rasterization. Rendering fidelity may differ per
delegate; semantics may not.

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

Platform-free components, unit-testable without any display.

### 3.1 `overlay_scene.{hpp,cpp}` — retained scene (source of truth)

- Verbs: `add(Shape) -> ShapeId`, `update(ShapeId, Shape)`, `remove(ShapeId)`,
  `clear()`. `ShapeId` is generation-scoped like every other grab id.
- Z-order: insertion order within two bands (`Band::Annotation` below
  `Band::Trail`), explicit `z` override per shape.
- Lifetime policy per shape (closed enum, **declarative — the delegate owns
  time**): `Persistent`, `Ttl{ms}`, `Fade{ms}` (alpha ramps to zero, then
  auto-removes). The scene records policies; it does not tick them. Expiry
  bookkeeping in the scene is lazy (a `Ttl`/`Fade` shape is considered gone
  after its deadline for query/resync purposes, no timer needed).
- Output: an ordered stream of `SceneDelta`s (`Upsert{ShapeId, Shape}`,
  `Remove{ShapeId}`, `Clear`), plus `snapshot()` returning the full live
  scene for delegate (re)attachment — same resync semantics as TreeStore
  (gap ⇒ full snapshot). Upserts are idempotent by id+generation.

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

### 3.3 `overlay_raster.{hpp,cpp}` — reference renderer (a library, not the contract)

In-tree anti-aliased scanline rasterizer: scene → premultiplied-ARGB
`grab::Image` + damage rects. It is **not** part of the SPI — it exists for
delegates that have no native scene graph (the X11 delegate uses it
internally) and as the normative rendering reference for golden-frame tests
(§9). No new dependencies (dependency policy: the PNG codec precedent — we
own pixel code).

### 3.4 `trail_animator.{hpp,cpp}` — the trail as a scene client

Consumes origin-stamped pointer-motion events from the bus subscription
surface; appends `Path` segments carrying `Fade{fade_ms}` into the `Trail`
band: red = `EventOrigin::Physical`, blue = `EventOrigin::InjectedSelf`
(defaults; CLI-overridable). Default fade 1200 ms, width 3 px. The animator
emits at motion rate and never ticks a frame — fading is the delegate's job.
Motion coalescing on the bus under backpressure is acceptable and
intentionally visible — the trail shows what subscribers actually see.

### 3.5 Pacing

The kernel runs **no render loop**. A delegate that must self-render (X11)
drives its own frame tick (target 60 Hz, damage-gated) with the existing
`PacingGovernor` + reactor timer inside the driver. No raw sleeps
(invariant #14) — the rule binds delegates too.

## 4. SPI: `src/spi/overlay_delegate.hpp`

```cpp
class OverlayDelegate
{
  public:
    virtual Result<void> open( CoordinateSpaceId space ) = 0;
    virtual Result<void> apply( std::span<const SceneDelta> deltas ) = 0;
    virtual Result<void> resync( const SceneSnapshot& scene ) = 0;
    virtual void         close() = 0;
};
```

- The contract is the **abstract scene**, not pixels: shape records with
  style, space-tagged geometry, and declarative lifetime/animation policies.
  How they become pixels — and on whose frame clock they fade — is entirely
  the delegate's business (mutter-composited ARGB window, GNOME Shell stage,
  Direct2D, CoreAnimation).
- `open` declares the delegate's coordinate space (its transform is
  registered in the space graph by the providing runtime); the kernel
  transforms shape geometry into that space before `apply`.
- `resync` replaces delegate state with a full snapshot — used at attach and
  after any gap (same law as TreeStore: gap ⇒ resnapshot, never a diff
  guess).
- The rendered surface is click-through and top-most by contract; a delegate
  that cannot guarantee both must not register the capability.
- `Runtime` gains `virtual OverlayDelegate* overlay_delegate()` (nullptr when
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

Every delegate renders the abstract scene with its native machinery; only
delegates lacking a native scene graph reuse the in-tree reference renderer.

| Path | Mechanism | Status |
|---|---|---|
| `src/drivers/desktop/x11/overlay_delegate.cpp` | ARGB override-redirect window on the composited screen; `_NET_WM_STATE_ABOVE`; XFixes empty *input* region (click-through); renders via the reference renderer + XShm blit on its own PacingGovernor-driven tick | **Phase 1 (now)** |
| `src/drivers/desktop/wayland/gnome_shell_delegate.cpp` | thin JS GNOME Shell extension drawing/animating on the Clutter stage (native fades); receives **scene deltas** over the daemon socket; the extension owns rendering, zero semantics; serves GNOME X11 *and* Wayland | later (with WP10) |
| `src/drivers/desktop/wayland/layer_shell_delegate.cpp` | `zwlr_layer_shell_v1` overlay layer + `wl_shm`, empty input region; reference renderer + own tick | later (KDE/wlroots) |
| `src/drivers/desktop/win32/overlay_delegate.cpp` | `WS_EX_LAYERED\|WS_EX_TRANSPARENT\|WS_EX_TOPMOST`; renders scene natively via Direct2D (its own fades) | later |
| `src/drivers/desktop/macos/overlay_delegate.cpp` | borderless `NSWindow` at overlay level, `ignoresMouseEvents`; CoreAnimation layers animate fades natively | later |

The X11 provider must handle: RandR geometry changes (reopen at new size,
space graph updated), compositor absence (no ARGB visual → typed
`CapabilityUnavailable`, never a black rectangle). Captures include the
overlay by default — seeing the annotation in the screenshot is usually the
point. An opt-out (`capture --without-overlay`, implemented as hide →
capture → show around the capture critical section via a `set_visible()`
on the delegate) is Phase 2.

## 8. Error handling

Existing taxonomy only: `CapabilityUnavailable` (no provider / no compositor),
`DisplayUnavailable`, plus typed staleness on `ShapeId` reuse after `clear()`
(generation mismatch). No new error codes anticipated; if implementation
finds a gap, the code is added to the descriptor table with disposition data
per invariant #15.

## 9. Testing

1. **Scene unit tests** — id/generation semantics, delta emission order,
   lazy expiry, `snapshot()`/resync equivalence (applying the delta stream
   and applying the snapshot yield the same scene).
2. **Delegate conformance (fake delegate)** — a recording `OverlayDelegate`
   asserts the SPI contract from the consumer side: idempotent upserts,
   gap ⇒ resync, space-transformed geometry.
3. **Reference renderer golden frames** — scripted scenes rendered and
   compared against committed PNGs (decoded by our own codec); antialiasing
   tolerance band documented in the test. Golden frames bind the *reference
   renderer only* — native delegates may differ in fidelity, never in
   which shapes exist where.
4. **Trail animator** — scripted motion event sequences (both origins) →
   expected segment set with `Fade` policies; coalescing-under-backpressure
   behavior pinned.
5. **Ring-3 self-verifying (Xvfb)** — the flagship test: open a session,
   draw a rect via the public API, run the trail, synthesize a drag through
   grab's own input path, capture through grab's own capture path, assert
   rect pixels and blue trail pixels along the drag line. Overlay, input,
   and capture dogfooded in one test.
6. **Invariant CI** — `overlay.hpp` added to the public-header allowlist in
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
