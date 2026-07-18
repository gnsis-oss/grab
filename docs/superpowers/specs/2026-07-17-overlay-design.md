# Spec: overlay — click-through annotation surface (scene, rasterizer, trail)

- **Date:** 2026-07-17
- **Status:** rev3 — Codex (`gpt-5.6-sol`) adversarial review: 27 findings, 24
  folded (revisioned delta envelope, lifetime metadata, X11 stacking/compositor
  corrections, capture matrix, flush fence, ownership ruling), 1 rejected
  (public-header allowlist check *does* exist: `check_public_headers.sh`), 2
  partially accepted (no Phase-1 multi-client broker — per-Session ownership
  instead; "top-most" contract qualified rather than dropped).
  **Phase 1 IMPLEMENTED 2026-07-18** on `feat/grab-port` (plan:
  `docs/superpowers/plans/2026-07-17-overlay-phase1.md`; commits
  `584ed14..c96d3ca`). Two implementation-time amendments: §6
  `Result<Overlay*>` (expected cannot hold references) and Task 8a —
  `MouseMove` gained an optional space-tagged `position` stamped at the X11
  provider edge (the event vocabulary carried only relative deltas; user
  ruling 2026-07-18). Compositor-dependent ring-3 positives skip on hosts
  without picom/xcompmgr.
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

- Verbs (all `[[nodiscard]] Result`-returning): `add(Shape) -> Result<ShapeId>`,
  `update(ShapeId, Shape) -> Result<void>`, `remove(ShapeId) -> Result<void>`,
  `clear()`. `ShapeId = {scene_epoch, slot}`; `clear()` atomically increments
  the epoch, so every pre-clear id yields a typed staleness error on reuse
  (table-backed code, reusing the existing staleness family if a fitting code
  exists, else one new descriptor row per invariant #15).
- **Revisions:** every mutation gets a monotonically increasing `revision`
  within the current epoch. `update` does not reset a shape's lifetime unless
  the lifetime policy itself is replaced.
- Z-order: insertion order within two bands (`Band::Annotation` below
  `Band::Trail`), explicit `z` override *within* a band only; ties break by
  insertion order.
- Lifetime policy per shape (closed enum): `Persistent`, `Ttl{ms}`,
  `Fade{ms}`. **The kernel owns logical time; delegates own scheduling**:
  each non-persistent shape record carries kernel-stamped `started_at`
  (session-monotonic ms) alongside its policy, so a delegate — including one
  attaching mid-life via resync — computes remaining duration instead of
  restarting it. The scene does not tick timers; expiry is bookkept via a
  deadline index drained opportunistically on every mutation/query (bounding
  retained state even for a motion-rate trail; revision and staleness
  semantics are preserved for drained shapes).
- Output: an ordered stream of enveloped `SceneDelta`s —
  `{scene_epoch, revision, Upsert{ShapeId, Shape} | Remove{ShapeId} | Clear}` —
  plus `snapshot()` returning the full live scene stamped
  `{scene_epoch, through_revision}`. Consumers must apply revisions
  contiguously within an epoch; any discontinuity or epoch change is a gap ⇒
  full `resync` (same law as TreeStore), resuming deltas after
  `through_revision`. Duplicate or older revisions are ignored (this, not
  bare id+generation, is what makes upserts idempotent and replay-safe).

### 3.2 Primitive vocabulary (closed, table-driven)

| Shape | Geometry | Notes |
|---|---|---|
| `Path` | ordered command list `Move / Line / Bezier / Close` (bezier control points per `geometry/curve.hpp` forms) | open or closed; the closed grammar, not "points and/or curves" |
| `Rect` | `geometry/rectangle.hpp` | |
| `Ellipse` | center + radii | circle = equal radii |
| `Polygon` | point list, closed | |

Style on every shape: stroke `{color RGBA, width px}`, optional fill
`{color RGBA}`. **Rendering rules are closed so delegates cannot diverge
semantically:** fill rule = nonzero winding; stroke caps/joins = round;
stroke width is *device-space* pixels (unscaled by geometry transforms);
fade interpolation = linear alpha over the remaining duration; invalid
geometry (NaN/inf, negative radii, empty path) is rejected at `add`/`update`
with `InvalidArgument`, never passed to a delegate.

**Coordinates:** every point/rect in a shape carries its `CoordinateSpaceId`;
untagged coordinates do not exist in the API (invariant #3). **The transform
boundary is `apply`:** the kernel transforms all geometry into the delegate's
declared space through the space graph *before* emitting deltas — delegates
never see foreign spaces. Affine transforms with rotation/shear lower `Rect`,
`Ellipse`, and `Polygon` to `Path` at the boundary; axis-aligned scale +
translation keep their shape type.

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
The animator consumes the subscription variant honestly: a `QueueGapMarker`,
an origin change, or a coordinate-space/generation change **breaks the path**
— it never fabricates a segment connecting samples across a discontinuity.
Scene mutation happens via reactor handoff, never inside the subscription
callback.

### 3.5 Pacing

The kernel runs **no render loop**. A delegate that must self-render (X11)
drives its own frame tick (target 60 Hz) with `PacingGovernor` + reactor
timer inside the driver. Damage gating must treat **every active fade as
continuing damage** and schedule `Ttl` expiry deadlines as timer events —
naive "no new deltas ⇒ no redraw" gating would freeze animations. Ticks
advance from the prior deadline (no drift). No raw sleeps (invariant #14) —
the rule binds delegates too. Named-concern hygiene: `PacingGovernor`
relocates from `src/kernel/capture/` to `src/kernel/scheduling/` in Phase 1
so presentation does not include a capture concern; capture re-points.

## 4. SPI: `src/spi/overlay_delegate.hpp`

```cpp
class OverlayDelegate
{
  public:
    virtual ~OverlayDelegate() = default;
    [[nodiscard]] virtual Result<void> open( CoordinateSpaceId space ) = 0;
    [[nodiscard]] virtual Result<void> apply( std::span<const SceneDelta> deltas ) = 0;
    [[nodiscard]] virtual Result<void> resync( const SceneSnapshot& scene ) = 0;
    [[nodiscard]] virtual Result<void> flush( Revision through ) = 0;
    virtual void                       close() = 0;
};
```

Lifecycle and threading contract: state machine
`Closed → open → Synced ↔ Desynced → close` with idempotent `close`;
delegates must consume or deep-copy spans before returning; calls arrive
serialized on the session's reactor thread (X11 affinity). **Failure rule:**
any `apply` failure marks the delegate desynchronized — further deltas are
rejected until an atomic `resync` succeeds. Public API success means *scene
acceptance*, not pixels; `flush(revision)` is the presentation fence — it
returns once the delegate has presented everything up to `revision` and the
X server has processed it. **An external compositor's repaint is
asynchronous and NOT fenced** — X11 offers no portable present-feedback for
another client's redraw; pixel-verifying consumers (the ring-3 tests)
bounded-retry their capture rather than assuming a stronger guarantee.
A `Clear` delta opening a new epoch (revision 1 of that epoch) is applied
atomically as the explicit epoch transition — never a gap; any other epoch
change or revision discontinuity desynchronizes.

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
- The rendered surface is **click-through unconditionally** (a delegate that
  cannot guarantee it must not register the capability) and **top-most within
  what its platform can promise** — on X11 that means above all *managed*
  windows with restacking on stacking changes; absolute precedence over other
  override-redirect/security windows is not promised anywhere and the
  contract says so.
- `Runtime` gains `[[nodiscard]] virtual OverlayDelegate* overlay_delegate()
  { return nullptr; }` — exact existing optional-accessor form, symmetric
  with `InputSeat*`/`TreeSource*`.
- **Ownership (Phase 1 ruling):** one scene per `Session`; the session leases
  its runtime's delegate for the session lifetime; `clear()` clears *that
  session's* scene only. Two sessions in one process get independent
  scenes/windows; cross-session or cross-client merged ordering is explicitly
  a non-goal until the daemon owns a broker-scene (Phase 2, where per-client
  namespaces and connection-death cleanup are specified). The CLI is simply a
  session holder.

## 5. Capability + resolution

New row `Capability::Overlay` in the existing enum/table (name string
`capability::overlay`). Resolution goes through the provider registry like
capture/input, with real availability probing at registration: on X11 the
prerequisites are an ARGB32 XRender visual, XFixes shape-input support, and
a live compositing manager (`_NET_WM_CM_Sn` selection *ownership*, not
merely an ARGB visual). Failure surfaces the existing
`ErrorCode::CapabilityUnavailable` with resolver reasons (doctor shows it
like any other capability).

## 6. Public API + CLI

- New public header `include/grab/overlay.hpp` (allowlist addition):
  `session.overlay() -> Result<Overlay*>` (expected cannot hold references)
  exposing the scene verbs, plus `Overlay::space()` — the coordinate space
  the surface renders in; the CLI parses geometry space-unresolved and
  stamps this space at the session boundary.
  **Type home rule (public headers cannot include `src/`):** the shape/style/
  lifetime *value types* and the `SceneDelta`/`SceneSnapshot` envelopes are
  defined in this public header (the `event.hpp` pattern — vocabulary as
  public value types); `src/spi/overlay_delegate.hpp` and the kernel scene
  include the public header, never the reverse. Only scene storage/algorithms
  live in `src/kernel/presentation/`. Nothing public names a platform.
- CLI (`src/frontends/cli/overlay_command.cpp`):
  - `grab overlay trail [--color --injected-color --fade-ms --width]` — runs
    the animator until interrupted. `grab trail` is an alias.
  - `grab overlay rect|ellipse|path --at ... [--ttl ms | --fade ms | --hold]`
    — one-shot annotations. **Lifetime rule:** a one-shot verb keeps the
    session (and thus the delegate window) alive until its shape's
    `Ttl`/`Fade` policy completes, then exits; default when no policy flag is
    given is `--ttl 3000`. `--hold` keeps `Persistent` shapes up until
    Ctrl-C. Annotations never silently vanish at process exit before their
    declared lifetime.
- Frontends compose public services only; no driver includes (dependency
  rule, CI-checked).

## 7. Providers (folder abstraction)

Every delegate renders the abstract scene with its native machinery; only
delegates lacking a native scene graph reuse the in-tree reference renderer.

| Path | Mechanism | Status |
|---|---|---|
| `src/drivers/desktop/x11/overlay_delegate.cpp` | override-redirect ARGB32 (XRender-picked visual + matching colormap) window; **self-restacks to top on stacking-change events** (`_NET_WM_STATE_ABOVE` is meaningless on unmanaged windows and is not used); XFixes zero-rect `ShapeInput` region applied after *every* window (re)creation (click-through); renders via the reference renderer, converted once into the visual's native `XImage` layout (masks/byte-order/stride), XShm blit with `XPutImage` fallback when MIT-SHM is absent; own PacingGovernor-driven tick; renderer keeps prior-frame state so damage = union of old+new bounds and removed shapes are cleared to transparent (no ghosts) | **Phase 1 (now)** |
| `src/drivers/desktop/wayland/gnome_shell_delegate.cpp` | thin JS GNOME Shell extension drawing/animating on the Clutter stage (native fades); receives **scene deltas** over the daemon socket; the extension owns rendering, zero semantics; serves GNOME X11 *and* Wayland | later (with WP10) |
| `src/drivers/desktop/wayland/layer_shell_delegate.cpp` | `zwlr_layer_shell_v1` overlay layer + `wl_shm`, empty input region; reference renderer + own tick | later (KDE/wlroots) |
| `src/drivers/desktop/win32/overlay_delegate.cpp` | `WS_EX_LAYERED\|WS_EX_TRANSPARENT\|WS_EX_TOPMOST`; renders scene natively via Direct2D (its own fades) | later |
| `src/drivers/desktop/macos/overlay_delegate.cpp` | borderless `NSWindow` at overlay level, `ignoresMouseEvents`; CoreAnimation layers animate fades natively | later |

The X11 delegate must additionally handle:

- **Compositor lifecycle, not just presence.** Capability requires
  `_NET_WM_CM_Sn` selection *ownership* at open (an ARGB visual alone proves
  nothing) and the owner is monitored afterward: if compositing is lost while
  mapped, the delegate immediately unmaps, reports desynchronized, and the
  capability goes unavailable — a fullscreen ARGB window without a compositor
  renders opaque, and "never a black rectangle" is a hard rule.
- **RandR as an atomic topology transaction:** update/bump the coordinate
  space, recreate the window at the new geometry, reapply the zero-rect
  input region, then full `resync` with retransformed geometry and
  full-surface damage. Retained shapes survive; their pixels are recomputed.
- **Capture matrix (corrected claim):** only *display* capture of the
  composited output includes the overlay; per-window capture
  (XComposite named-window pixmap) of a target window **never** contains the
  sibling overlay window. The spec claims inclusion only for display capture;
  window captures are documented as overlay-free. The Phase-2 opt-out
  (`capture --without-overlay` via hide → capture → show with a
  `set_visible()` on the delegate) therefore applies to display capture only.

## 8. Error handling

Existing taxonomy where it fits: `CapabilityUnavailable` (no provider /
no compositor / no XFixes shape-input), `DisplayUnavailable`,
`InvalidArgument` (rejected geometry), plus typed staleness on `ShapeId`
use after `clear()` (`scene_epoch` mismatch) — reuse an existing staleness
code if one fits, else one new table-backed row with disposition data per
invariant #15.

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
5. **Ring-3 self-verifying (Xvfb + compositor)** — the flagship test.
   **Bare Xvfb has no compositor, so the positive test would fail by
   construction**; the fixture therefore runs Xvfb *plus* a compositing
   manager (`picom` or `xcompmgr`, whichever the fixture finds — a test-only
   dependency, recorded as such; test skips with a clear reason if neither
   exists). Flow: open a session, draw a rect via the public API, run the
   trail, synthesize a drag through grab's own input path, `flush()` the
   presentation fence, capture via **display capture** (per the capture
   matrix), assert rect pixels and blue trail pixels along the drag line —
   and assert a sentinel window beneath the overlay *received* the drag
   (click-through proven, not assumed). Overlay, input, and capture
   dogfooded in one test.
6. **Ring-3 negative (bare Xvfb)** — no compositor ⇒ resolution yields
   `CapabilityUnavailable` with the compositor reason; nothing is mapped.
7. **RandR resize (Xvfb + compositor)** — resize the virtual output
   mid-scene; assert retained shapes reappear at retransformed positions and
   the input region is still empty.
8. **Invariant CI** — `overlay.hpp` added to the public-header allowlist in
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
