# Overlay Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement Phase 1 of `docs/superpowers/specs/2026-07-17-overlay-design.md` (rev3): the abstract overlay scene + reference renderer in the kernel, the `OverlayDelegate` SPI, the X11 delegate, `Capability::Overlay`, public `include/grab/overlay.hpp` with `Session::overlay()`, `grab overlay` CLI verbs, and the ring-3 test suite.

**Architecture:** Kernel owns the vocabulary and scene truth (revisioned, epoch-scoped `SceneDelta` stream + snapshot/resync; kernel-stamped lifetime clocks); delegates own rendering and time. The X11 delegate self-renders via the in-tree reference renderer into an override-redirect ARGB window (click-through via XFixes, compositor-gated). The trail is one scene client consuming origin-stamped motion.

**Tech Stack:** C++23, Clang, CMake/Ninja, GTest, XCB (+ XFixes/XShm/XRender/RandR), existing grab kernel (EventBus, space graph, reactor, registry).

## Global Constraints

- Spec of record: `docs/superpowers/specs/2026-07-17-overlay-design.md` (rev3). On divergence, fix spec or plan in the same commit.
- Branch/worktree: `feat/grab-port` in `grab/.worktrees/integrate`. Never the primary checkout.
- Git identity: repo-configured user only; **no co-author/AI attribution ever**; verify per task: `git log --format='%an <%ae> / %cn <%ce>' -1`.
- Build discipline: `cmake --build build -j$(nproc)` + `ctest --test-dir build --output-on-failure` green before every commit; X11 flakes re-run serially before judging.
- Naming: CamelCase enum values, no `k` prefix, private members trailing `_`, `EnumTable` + count `static_assert`s for closed vocabularies.
- No raw sleeps anywhere (invariant #14; `tests/scripts/sleep_allowlist.txt` is shrink-only). No `(0,0)` sentinels; unknown is typed absence. Public headers never include `src/` (CI-checked).
- `docs/` is gitignored — commit docs with `git add -f`.
- Namespaces: public value types `grab::overlay`, scene/renderer `grab::kernel::presentation`, SPI `grab::spi`, X11 `grab::drivers::desktop::x11`.

---

### Task 1: Public vocabulary — `include/grab/overlay.hpp` + `Capability::Overlay`

**Files:**
- Create: `include/grab/overlay.hpp`
- Modify: `include/grab/capability.hpp` (enum + `enum_table` row + count static_assert), `tests/scripts/public_header_allowlist.txt` (+`overlay.hpp`)
- Test: `tests/core/test_overlay_types.cpp` (register in the `grab_core_tests` executable, `tests/CMakeLists.txt`)

**Interfaces:**
- Produces (exact types every later task consumes):

```cpp
namespace grab::overlay
{
    struct Color { std::uint8_t r = 0U, g = 0U, b = 0U, a = 255U; };
    struct StrokeStyle { Color color{}; float width_px = 1.0F; };   // device-space px (spec §3.2)
    struct FillStyle { Color color{}; };

    struct Persistent {};
    struct Ttl  { std::chrono::milliseconds duration{}; };
    struct Fade { std::chrono::milliseconds duration{}; };
    using LifetimePolicy = std::variant<Persistent, Ttl, Fade>;

    enum class Band : std::uint8_t { Annotation, Trail };

    struct MoveTo   { SpacePoint point{}; };
    struct LineTo   { SpacePoint point{}; };
    struct BezierTo { std::vector<SpacePoint> control; };   // control points, curve.hpp evaluation forms
    struct ClosePath {};
    using PathCommand = std::variant<MoveTo, LineTo, BezierTo, ClosePath>;

    struct Path    { std::vector<PathCommand> commands; bool closed = false; };
    struct Rect    { SpaceRect bounds{}; };
    struct Ellipse { SpacePoint center{}; double radius_x = 0.0, radius_y = 0.0; };
    struct Polygon { std::vector<SpacePoint> points; };
    using Geometry = std::variant<Path, Rect, Ellipse, Polygon>;

    struct Shape
    {
        Geometry                   geometry{};
        std::optional<StrokeStyle> stroke{};
        std::optional<FillStyle>   fill{};
        LifetimePolicy             lifetime{ Persistent{} };
        Band                       band = Band::Annotation;
        std::int32_t               z    = 0;
    };

    struct SceneEpoch { std::uint64_t value = 0U; auto operator<=>( const SceneEpoch&, const SceneEpoch& ) = default; };
    struct Revision   { std::uint64_t value = 0U; auto operator<=>( const Revision&, const Revision& ) = default; };
    struct ShapeId    { SceneEpoch epoch{}; std::uint32_t slot = 0U; auto operator<=>( const ShapeId&, const ShapeId& ) = default; };

    struct ShapeRecord { ShapeId id{}; Shape shape{}; std::chrono::milliseconds started_at{}; };  // session-monotonic

    struct Upsert { ShapeRecord record{}; };
    struct Remove { ShapeId id{}; };
    struct Clear  { SceneEpoch new_epoch{}; };
    struct SceneDelta { SceneEpoch epoch{}; Revision revision{}; std::variant<Upsert, Remove, Clear> change{}; };

    struct SceneSnapshot { SceneEpoch epoch{}; Revision through_revision{}; std::vector<ShapeRecord> shapes; };
}
```

- Includes allowed: `grab/space.hpp`, `grab/geometry/*.hpp`, std only. **No `src/` includes** (allowlist + invariant CI enforce).

- [ ] **Step 1: Write the failing test** — `tests/core/test_overlay_types.cpp` with named constants, `static_assert`-style checks inside `TEST()` bodies (repo pattern for type-level tests): variant alternative counts (`std::variant_size_v<Geometry> == 4`), `ShapeId` ordering, aggregate default values (`Color{}.a == 255`), `SceneDelta` holds all three change alternatives.
- [ ] **Step 2: Run to verify failure** — `cmake --build build -j$(nproc)` → expected: compile error `grab/overlay.hpp: No such file`.
- [ ] **Step 3: Implement `include/grab/overlay.hpp`** exactly as the Interfaces block (plus header guard pragma + includes, matching `watch.hpp` style).
- [ ] **Step 4: Add `Capability::Overlay`** — new enum value after the last existing row in `include/grab/capability.hpp`, matching `enum_table` row `enum_entry( Capability::Overlay, capability::overlay )` with `inline constexpr std::string_view overlay = "overlay";` in the names namespace; bump the count `static_assert`. Add `overlay.hpp` to `tests/scripts/public_header_allowlist.txt` (sorted position).
- [ ] **Step 5: Build + full ctest green** — `cmake --build build -j$(nproc) && ctest --test-dir build -R 'grab_core_tests|grab_public_header_allowlist|grab_invariant_checks' --output-on-failure` → PASS.
- [ ] **Step 6: Commit** — `git add include/grab/overlay.hpp include/grab/capability.hpp tests/scripts/public_header_allowlist.txt tests/core/test_overlay_types.cpp tests/CMakeLists.txt && git commit -m "feat(overlay): public vocabulary — shapes, lifetime, revisioned delta envelope; Capability::Overlay"`.

### Task 2: Relocate `PacingGovernor` to `src/kernel/scheduling/`

**Files:**
- Move: `src/kernel/capture/pacing_governor.{hpp,cpp}` → `src/kernel/scheduling/pacing_governor.{hpp,cpp}` (`git mv`)
- Modify: namespace `grab::kernel::capture` → `grab::kernel::scheduling` in both files; every includer/user (`grep -rln 'capture/pacing_governor\|capture::PacingGovernor' src tests` — re-point includes and qualify uses); `CMakeLists.txt` `grab_kernel` source list.
- Test: existing pacing tests move with it (`grep -rl PacingGovernor tests`).

**Interfaces:**
- Produces: `grab::kernel::scheduling::PacingGovernor` — API unchanged: `for_fps(std::uint32_t) -> Result<PacingGovernor>`, `interval() -> std::chrono::nanoseconds`, `next_deadline(time_point) -> time_point`.

- [ ] **Step 1:** `git mv` both files; apply namespace + include fixups everywhere the grep finds.
- [ ] **Step 2:** Build + full suite — `cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure` → green (pure relocation, zero behavior change).
- [ ] **Step 3: Commit** — `git commit -am "refactor(kernel): PacingGovernor to scheduling — presentation must not include a capture concern"`.

### Task 3: Kernel scene — `OverlayScene`

**Files:**
- Create: `src/kernel/presentation/overlay_scene.{hpp,cpp}`
- Modify: `CMakeLists.txt` (`grab_kernel` sources)
- Test: `tests/kernel/test_overlay_scene.cpp` (register in `grab_kernel_tests`)

**Interfaces:**
- Consumes: Task 1 types.
- Produces:

```cpp
namespace grab::kernel::presentation
{
    class OverlayScene final
    {
      public:
        using Clock     = std::function<std::chrono::milliseconds()>;   // session-monotonic
        using DeltaSink = std::function<void( const overlay::SceneDelta& )>;

        explicit OverlayScene( Clock clock );

        [[nodiscard]] Result<overlay::ShapeId> add( overlay::Shape shape );
        [[nodiscard]] Result<void>             update( overlay::ShapeId id, overlay::Shape shape );
        [[nodiscard]] Result<void>             remove( overlay::ShapeId id );
        void                                   clear();
        [[nodiscard]] overlay::SceneSnapshot   snapshot() const;
        void                                   set_delta_sink( DeltaSink sink );
    };
}
```

Semantics (spec §3.1, all tested): epoch bump + `Clear` delta on `clear()`; per-mutation monotonic revision within the epoch; stale `ShapeId` (old epoch or removed slot) → typed staleness error (reuse the existing staleness code the descriptor table already carries for refs — locate via `grep -n 'Stale' include/grab/result.hpp` — else add ONE new row `StaleShape` with disposition data); validation at `add`/`update` (`InvalidArgument`: NaN/inf coordinate, negative radius, empty path, neither stroke nor fill); `started_at` stamped from `Clock` on add (and on update only when the lifetime policy alternative is replaced); expiry deadline index drained on every mutation/`snapshot()` (drained shapes emit `Remove` deltas; their ids stay stale, revisions stay monotonic); `update` on an expired-and-drained shape → staleness error.

- [ ] **Step 1: Write failing tests** (named constants; fake clock `std::chrono::milliseconds` variable captured by the lambda): (a) add→snapshot roundtrip carries `started_at` from the fake clock; (b) revisions strictly increase across add/update/remove; delta stream replayed onto an empty map equals `snapshot()` (snapshot≡delta equivalence); (c) `clear()` bumps epoch, emits `Clear{new_epoch}`, old id → staleness error; (d) duplicate `update` emits a new revision (no dedup at scene level — replay-safety is the *consumer's* duty, tested in Task 4); (e) advancing the fake clock past a `Ttl{50ms}` shape then calling `snapshot()` drains it: snapshot excludes it, a `Remove` delta was emitted, `update` on it errors; (f) each invalid-geometry case → `InvalidArgument`, no delta emitted.
- [ ] **Step 2: Run to verify failure** — `cmake --build build -j$(nproc)` → unresolved `OverlayScene`.
- [ ] **Step 3: Implement** — flat storage (`std::vector<Entry>` + free-slot list, no node containers), `Entry{ShapeRecord record; bool live;}`; deadline min-index as a sorted flat vector; emit deltas after state commit, sink called outside any lock (EventBus publish-outside-lock precedent).
- [ ] **Step 4: Run tests** — `ctest --test-dir build -R grab_kernel_tests --output-on-failure` → PASS.
- [ ] **Step 5: Commit** — `git commit -m "feat(overlay): kernel OverlayScene — epochs, revisions, kernel-stamped lifetime, lazy expiry drain"`.

### Task 4: SPI — `OverlayDelegate` + fake + conformance tests

**Files:**
- Create: `src/spi/overlay_delegate.hpp`; `tests/fake/fake_overlay_delegate.hpp`
- Modify: `src/spi/runtime.hpp` (accessor)
- Test: `tests/kernel/test_overlay_delegate_contract.cpp` (in `grab_kernel_tests`)

**Interfaces:**
- Consumes: Task 1 envelope types.
- Produces:

```cpp
namespace grab::spi
{
    class OverlayDelegate
    {
      public:
        OverlayDelegate() = default;
        virtual ~OverlayDelegate() = default;
        OverlayDelegate( const OverlayDelegate& ) = delete;
        OverlayDelegate& operator=( const OverlayDelegate& ) = delete;
        OverlayDelegate( OverlayDelegate&& ) = delete;
        OverlayDelegate& operator=( OverlayDelegate&& ) = delete;

        [[nodiscard]] virtual Result<void> open( CoordinateSpaceId space )                       = 0;
        [[nodiscard]] virtual Result<void> apply( std::span<const overlay::SceneDelta> deltas )  = 0;
        [[nodiscard]] virtual Result<void> resync( const overlay::SceneSnapshot& scene )         = 0;
        [[nodiscard]] virtual Result<void> flush( overlay::Revision through )                    = 0;
        virtual void                       close()                                               = 0;
    };
}
```

- `Runtime` gains `[[nodiscard]] virtual OverlayDelegate* overlay_delegate() { return nullptr; }` (exact optional-accessor form used by the other accessors in `src/spi/runtime.hpp`, with forward declaration).
- `FakeOverlayDelegate` (test-only): records every call; owns a shape map applying deltas with **revision-contiguity checking** — non-contiguous revision or epoch change ⇒ it reports desync and rejects further `apply` until `resync`; deep-copies spans.

**Contract rules encoded in the fake + tests (spec §4):** state machine `Closed → open → Synced ↔ Desynced → close`, idempotent `close`; any `apply` failure ⇒ Desynced ⇒ `apply` rejected until atomic `resync`; duplicate/older revisions ignored (idempotent replay); spans not retained.

- [ ] **Step 1: Write failing conformance tests** driving `OverlayScene` → `FakeOverlayDelegate`: (a) contiguous delta stream ⇒ fake's map == scene snapshot; (b) drop one delta ⇒ fake flags gap ⇒ `resync(snapshot())` heals ⇒ maps equal again; (c) replaying an already-applied delta is a no-op; (d) `apply` after injected failure is rejected until `resync`; (e) `close(); close();` is safe.
- [ ] **Step 2: Verify failure** — build → unresolved includes.
- [ ] **Step 3: Implement** header + fake + accessor.
- [ ] **Step 4:** `ctest --test-dir build -R grab_kernel_tests --output-on-failure` → PASS.
- [ ] **Step 5: Commit** — `git commit -m "feat(overlay): OverlayDelegate SPI, Runtime accessor, recording fake with revision-contiguity conformance"`.

### Task 5: Reference renderer — `OverlayRaster` + golden frames

**Files:**
- Create: `src/kernel/presentation/overlay_raster.{hpp,cpp}`
- Test: `tests/kernel/test_overlay_raster.cpp`; golden PNGs under `tests/kernel/golden/overlay/` (generated by the test binary with `--update-golden` on first run, then committed and diffed thereafter — the PNG codec encodes/decodes them)

**Interfaces:**
- Consumes: Task 1 shapes (geometry already in the surface's space), `grab::Image`.
- Produces:

```cpp
namespace grab::kernel::presentation
{
    struct RasterFrame { const Image& pixels; std::vector<geometry::Rectangle> damage; };   // device-space rects

    class OverlayRaster final
    {
      public:
        [[nodiscard]] static Result<OverlayRaster> create( geometry::Size size );
        // Stateful: damage = union of previous and current bounds of every changed
        // shape; removed shapes' old bounds are cleared to transparent (no ghosts).
        [[nodiscard]] Result<RasterFrame> render( std::span<const overlay::ShapeRecord> shapes,
                                                  std::chrono::milliseconds             now );
        [[nodiscard]] geometry::Size size() const noexcept;
    };
}
```

Rendering rules (spec §3.2, closed): nonzero winding fill; round caps/joins; device-space stroke width; linear alpha fade over remaining `Fade` duration (alpha 0 at/after deadline); band then `z` then insertion order; premultiplied ARGB output; anti-aliased edges (coverage-based scanline).

- [ ] **Step 1: Write failing golden tests** — scripted scenes: stroked rect; filled+stroked ellipse; open bezier path; polygon nonzero-winding self-overlap; `Fade` shape at t=0/half/expiry (3 frames); a remove step asserting the ghost region is transparent and damage covers old bounds. Assertions: pixel equality vs golden within documented per-channel tolerance ±2 (AA), damage-rect exactness.
- [ ] **Step 2: Verify failure**, **Step 3: Implement** (scanline coverage rasterizer: flatten beziers via `geometry::Curve::evaluate` at adaptive step; stroke as polygon expansion with round joins sampled at fixed angular step; keep `previous_bounds_` per slot for damage), **Step 4: Generate goldens with `--update-golden`, inspect, commit goldens, re-run without the flag → PASS.**
- [ ] **Step 5: Commit** — `git commit -m "feat(overlay): reference renderer — stateful AA scanline raster, closed rendering rules, golden frames"`.

### Task 6: Session integration — public `Overlay` facade

**Files:**
- Create: `src/kernel/presentation/overlay_service.{hpp,cpp}` (scene + delegate lease + transform-at-apply + desync/resync coordinator)
- Modify: `include/grab/overlay.hpp` (add public `class Overlay` verb facade), `include/grab/session.hpp` + `src/kernel/lifecycle/session_impl.{hpp,cpp}` (accessor + composition), spec §6 (signature correction, same commit)
- Test: `tests/kernel/test_overlay_service.cpp` (fake delegate + fake TreeNav-style space setup)

**Interfaces:**
- Produces: `Session::overlay() -> Result<Overlay*>` (non-owning, session lifetime — `Result<T&>` is not representable in `std::expected`; **amend spec §6 to `Result<Overlay*>` in this commit**, matching the `Result<Keymap*>` precedent). `Overlay` public verbs mirror the scene: `add/update/remove/clear/flush`.
- Behavior: resolves the session's runtime `overlay_delegate()`; absent ⇒ `CapabilityUnavailable` with resolver reason. Transforms shape geometry into the delegate's declared space through the space graph **before** `apply` (affine with rotation/shear lowers Rect/Ellipse/Polygon to Path; axis-aligned keeps type). Any `apply` failure ⇒ marks desynced ⇒ next verb triggers `resync(snapshot)` before its delta. Verb success = scene acceptance; `flush()` = presentation fence passthrough.

- [ ] **Step 1: Write failing tests** — (a) no delegate ⇒ `CapabilityUnavailable`; (b) verb → fake delegate receives space-transformed geometry; (c) injected apply failure ⇒ next verb resyncs first (fake records order); (d) two Sessions in one process → two independent scenes (ownership ruling).
- [ ] **Step 2: Verify failure**, **Step 3: Implement**, **Step 4: `ctest -R grab_kernel_tests` → PASS**, full suite green.
- [ ] **Step 5: Commit** — `git add -f docs/... && git commit -m "feat(overlay): Session::overlay() facade — delegate lease, transform-at-apply, desync/resync coordinator; spec: Result<Overlay*>"`.

### Task 7: X11 delegate

**Files:**
- Create: `src/drivers/desktop/x11/overlay_delegate.{hpp,cpp}`
- Modify: `src/drivers/desktop/x11/x11_runtime.{hpp,cpp}` (member + `overlay_delegate()` override + capability availability), the X11 capability registration site (`src/drivers/desktop/x11/x11_routes.cpp` — mirror the existing `Capability` rows there), `CMakeLists.txt` (`grab_driver_x11` sources; link `PkgConfig::GRAB_XCB_XFIXES`, `GRAB_XCB_RENDER`, `GRAB_XCB_SHAPE` — add to `cmake/Xcb.cmake` if not yet probed)
- Test: `tests/drivers/x11/test_overlay_delegate.cpp` (in `grab_x11_tests`, Xvfb fixture)

**Interfaces:** implements `spi::OverlayDelegate` per spec §7 X11 row. Order of implementation obligations:

1. **Probe (constructor/open):** XRender ARGB32 pictformat → visual + matching colormap; XFixes shape-input extension present; `_NET_WM_CM_S<screen>` selection **owned** (`xcb_get_selection_owner`). Any missing ⇒ `CapabilityUnavailable` with a distinct reason string; runtime does not advertise the capability.
2. **Window:** override-redirect, 32-bit depth, ARGB visual + colormap, full-screen at the space's geometry; zero-rect `ShapeInput` region via `xcb_shape_rectangles` applied **after every window creation** (helper `apply_input_passthrough()` called from create + RandR path); map; self-restack: subscribe `SubstructureNotify` on root, on sibling `ConfigureNotify`/`MapNotify` raise via `xcb_configure_window(XCB_STACK_MODE_ABOVE)`.
3. **Present:** `OverlayRaster` render → convert once into the visual's native `XImage` layout (masks/byte-order/stride from the returned visual, premultiplied) → `XShmPutImage` per damage rect; `XPutImage` fallback when MIT-SHM absent (probe once).
4. **Tick:** reactor timerfd via `scheduling::PacingGovernor::for_fps(60)`; damage gating counts every live `Fade` as continuing damage; `Ttl` deadlines scheduled as timer events; deadline advanced from prior deadline.
5. **`flush(revision)`:** blocks (bounded by context deadline) until the tick that presented `revision` completes + `xcb_flush` + sync round-trip (`xcb_get_input_focus` reply as fence).
6. **Compositor monitor:** watch `_NET_WM_CM_S<screen>` owner (XFixes selection notify); loss ⇒ unmap immediately, mark Desynced, availability withdrawn.
7. **RandR transaction:** on screen-change event: update space geometry (space graph bump via the coordinate authority), destroy+recreate window at new size, `apply_input_passthrough()`, force full `resync` + full-surface damage.

- [ ] **Step 1: Write failing tests** (Xvfb; compositor-dependent cases live in Task 9 — here only compositor-independent behavior): probe returns `CapabilityUnavailable` on bare Xvfb **with the compositor reason**; `apply_input_passthrough` sets an empty input region (`xcb_shape_get_rectangles` count == 0); open-close-open cycle safe.
- [ ] **Step 2: Verify failure**, **Step 3: Implement in the obligation order above** (each obligation its own commit-sized increment is acceptable; keep the suite green at each), **Step 4: tests PASS**, full suite green serially.
- [ ] **Step 5: Commit(s)** — `git commit -m "feat(overlay): X11 delegate — ARGB override-redirect, input passthrough, compositor gate, XShm present, paced self-render"`.

### Task 8: Trail animator

**Files:**
- Create: `src/kernel/presentation/trail_animator.{hpp,cpp}`
- Test: `tests/kernel/test_trail_animator.cpp`

**Interfaces:**
- Consumes: `SubscriptionEvent` variant (`grab/watch.hpp`: `std::variant<Event, QueueGapMarker>`), `Overlay`-facade-compatible sink (constructor takes `OverlayScene&` or the public `Overlay*` — take `OverlayScene&`, the CLI wires it through the service).
- Produces: `TrailAnimator{ OverlayScene&, TrailStyle{ Color physical, Color injected, std::chrono::milliseconds fade, float width_px } }` with `void consume( const SubscriptionEvent& )` — pure function of the input stream, reactor-handoff is the caller's duty.
- Rules (spec §3.4): motion event appends a `Fade`-tagged path segment in `Band::Trail`, colored by `EventOrigin`; **path breaks** (no connecting segment) on: `QueueGapMarker`, origin change, coordinate-space/generation change; non-motion events ignored.

- [ ] **Step 1: Write failing tests** — scripted `SubscriptionEvent` sequences: (a) 3 physical motions → 2 red segments with `Fade` and correct `started_at` ordering; (b) physical→injected transition → no bridging segment, next segment blue; (c) gap marker mid-stream → break; (d) space-id change → break.
- [ ] **Step 2: Verify failure**, **Step 3: Implement**, **Step 4: PASS + full suite**, **Step 5: Commit** — `git commit -m "feat(overlay): trail animator — origin-colored fading segments, honest gap/origin/space breaks"`.

### Task 9: CLI verbs + ring-3 suite

**Files:**
- Create: `src/frontends/cli/overlay_command.{hpp,cpp}`; `tests/integration/test_overlay_ring3.cpp`
- Modify: `src/frontends/cli/main.cpp` (verb table hookup — same pattern as `input_command`), `include/grab/command_descriptor.hpp` (rows `overlay.trail`, `overlay.shape` — `CommandKind` per existing taxonomy, `Mutability::Mutating`, `RetryClass::ResolveOnly`, non-idempotent, not consent-gated in-process), `tests/xvfb_fixture.sh` (compositor support), `tests/CMakeLists.txt`
- Test: ring-3 per spec §9 items 5–7.

**Behavior (spec §6):** `grab overlay trail [--color RRGGBB --injected-color RRGGBB --fade-ms N --width F]` runs until SIGINT (`grab trail` alias); `grab overlay rect|ellipse|path --at ... [--ttl ms | --fade ms | --hold]` — **default `--ttl 3000`**; process stays alive until the policy completes (reactor timer, no sleeps), `--hold` until SIGINT.

**Fixture:** `tests/xvfb_fixture.sh` gains `start_compositor()` — prefer `picom --backend xrender`, fall back to `xcompmgr`; if neither binary exists, positive tests `GTEST_SKIP` with reason `"no compositing manager installed (test-only dependency)"`. Bare-Xvfb negative test never needs one.

- [ ] **Step 1: Write failing ring-3 tests** — (spec §9 №5) Xvfb+compositor: session, public rect, trail on, synthesized drag via grab input path, `flush`, **display capture**, assert rect pixels + blue trail pixels along the drag segment + **sentinel window received the drag** (create an XCB sentinel window under the overlay recording button events); (№6) bare Xvfb: resolution ⇒ `CapabilityUnavailable` with compositor reason, nothing mapped; (№7) RandR resize mid-scene ⇒ shapes reappear retransformed, input region still empty.
- [ ] **Step 2: Verify failure**, **Step 3: Implement CLI + fixture**, **Step 4: full suite green (serial rerun for X11 flakes)**.
- [ ] **Step 5: Commit** — `git commit -m "feat(overlay): grab overlay CLI verbs + ring-3 suite (compositor fixture, negative gate, RandR, click-through sentinel)"`.

### Task 10: Close-out

- [ ] README capability/technology table row (`Overlay annotations | XFixes/XRender ARGB + compositor`); spec status flip to "Phase 1 implemented"; verify `grep -rn 'TODO\|Placeholder' src/kernel/presentation src/drivers/desktop/x11/overlay_delegate.cpp` is empty.
- [ ] Full suite + `grab_invariant_checks` + `grab_public_header_allowlist` green; authorship single-identity across the plan range: `git log --format='%an <%ae> / %cn <%ce>' <plan-base>..HEAD | sort -u` → exactly one line.
- [ ] Commit docs — `git add -f docs/... && git commit -m "docs(overlay): Phase 1 complete — spec status + README row"`.
