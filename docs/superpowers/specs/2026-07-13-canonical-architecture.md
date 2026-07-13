# Spec: grab canonical architecture — node/surface model, contracts, and phased adoption

- **Date:** 2026-07-13
- **Status:** draft rev2 (post-Codex-review; all 25 findings folded — see §0a)
- **Source of authority:** `/home/gn/ws/grab_workspace/ARCH-CANONICAL-PLAN-2026-07-13.md`
  (workspace root — outside this repo by design; context docs live at the
  workspace root). That document carries rationale, evidence, and rulings
  R1–R7; this spec translates it into repo-local, implementable contracts. On
  conflict, the canonical plan wins and this spec must be amended.
- **Scope:** Phase 0 (contracts + vendored l0 drop) and Phase 1 (node/surface
  kernel + spine as one X11 vertical slice) are specified to implementation
  precision. Phase 2 (Wayland session/lease) and Phase 3 (agent surface) are
  specified at contract-boundary level only; each gets its own spec before
  execution.
- **Amends the parents** `2026-07-03-grab-architecture-design.md` and
  `2026-07-06-eventgrab-mousegrab-into-grab-design.md`: the closed per-domain
  public API (`Screen::open` / `Input::open` verbs on raw window ids) becomes a
  convenience facade over a generic node/surface session model; `BrowserTab`
  and the browser classifier are demoted from canonical identity to a
  compatibility projection and a low-confidence evidence adapter.

## 0. Provenance

Derived from three inputs, in priority order:

1. `ARCH-CANONICAL-PLAN-2026-07-13.md` — the combined UiNode/Surface decision
   (2026-07-11) + reference-study consolidation (Claude + Codex reviews of ~50
   automation libraries, six-category adversarial comparison).
2. The current tree on `feat/grab-port` (this worktree), including its
   in-progress source reorganization.
3. The l0 libraries at rev `a8693bf4` (see §2).

Terminology used below without redefinition (see canonical plan Part I):
Locator, NodeRef, UiNodeRecord, UiSnapshot, Match, PinnedTarget, Surface,
TargetRegistry, facet, route, receipt, commit boundary, SessionLease.

## 0a. Review provenance (rev2)

A Codex (`gpt-5.6-sol`) adversarial review fact-checked rev1 against the repo,
the l0 sources, and the canonical plan; 25 findings, all folded. Load-bearing
corrections: Phase 0/1 re-scoped to carry every canonical WP0/WP1–WP9
obligation (rev1 silently dropped client/loopback, command registries, daemon
hardening, TileDiffer/InjectGate, ADRs, and several Phase-0 gates); error-code
count corrected (26 new, 45 total); transform model upgraded from scale-only
to affine (window→global needs translation); relation storage changed to
relation-set edges (vendored `Web` rejects parallel edges); `OwnedProcess`
adoption restricted to verified direct children; vendor logging shim corrected
to the real `logger::` symbols; two vendored-lib defects to patch on drop
(`walk::diff` endpoint-hash collision; `walk::graft` hides its id remap).
Partial acceptance: the reviewer objected to `xid` inside `WindowRef` as
backend leakage — the canonical plan's R4 specifies that field, so it stays,
documented as an opaque native token (finding otherwise absorbed via
runtime-scoped `WidgetRef` and `FrameRef`).

## 1. Governing principles (normative)

1. **No live universal node object.** Public targeting is `Locator` (immutable
   plan) → `Match` (resolved, evidenced) → generation-checked refs. Node data
   crosses the API only as immutable records inside revisioned snapshots.
2. **No merged live graph.** Per-runtime snapshots + typed correlation edges;
   composite views are query-time projections. Title/PID/rect matches never
   fuse identity.
3. **Generations everywhere.** Every reference type carries a generation/epoch
   and yields a *typed* staleness error on stale use — never a wrong answer.
4. **Coordinates carry provenance.** Every public point/rect/frame is tagged
   with a `CoordinateSpaceId`; transforms are affine (or opaque native routes)
   and carry scale, mapping id, generation, trust. `(0, 0)`-style sentinels
   are banned; unknown is typed absence.
5. **Actions are transactions.** One wait/actionability engine; barriers armed
   before commit; an input-commit boundary (resolution/preconditions may retry,
   a possibly-committed action never auto-replays); every mutating verb returns
   a `Receipt`.
6. **Descriptor tables stay canonical** (events; extended to commands and
   errors) with namespaced extensions and tolerant decode — no open envelope,
   no second vocabulary.
7. **The provider registry stays**, amended with plan coherence (composed
   capabilities must share authority + coordinate mapping) and, on Wayland,
   session-scoped resolution output (`SessionLease`).
8. **Process authority requires ownership receipts.** `BorrowedProcessId`
   observes; only `OwnedProcess` (pidfd + start identity, direct children
   only) terminates.
9. **Vendored l0 stays internal.** `tag::`/`web::`/`walk::`/`heap::`/`out::`
   never appear in `include/grab/`.
10. The 18 invariants in canonical plan §13 are review gates for every PR this
    spec spawns.

## 2. Vendored l0 libraries (Phase 0)

Copy from `~/ws/l0` at rev `a8693bf4` (single repo; record the rev once) into
`src/vendor/l0/`, preserving upstream namespaces and layout:

| Vendor path | Source | Contents used |
|---|---|---|
| `src/vendor/l0/tag/` | `t1/tag_workspace/tag/include/tag/` | `Id<N>`, `Idx<Tag,Rep>`, `IdLane<N>`, RFC 9562 generators (`gen.hpp`: free functions `tag::random/named/blend/timed`), `rng.hpp` (`FastRng`/`SafeRng`), `ns.hpp`, `ContentId`, `detail/sha1.hpp`. `peer.hpp`, `context.hpp`, `fast/pool.hpp` copied but unused until the daemon work. |
| `src/vendor/l0/web/` | `t2/web_workspace/web/include/web/` | `web.hpp`, `knot.hpp`, `trait.hpp`, `concept.hpp`, `fast/csr.hpp`. **`syntax/` is NOT copied** (depends on `stave`; its parallel-array pattern is reimplemented in the TreeStore). |
| `src/vendor/l0/walk/` | `t3/walk_workspace/walk/include/walk/` | all headers (`sweep`, `delve`, `path`, `rank`, `diff`, `graft`, `knot`, `span`, `strategy`, `visitor`, `detail/union_find`, …). |
| `src/vendor/l0/heap/` | `t2/heap_workspace/heap/include/heap/` | all (walk::path dependency). |
| `src/vendor/l0/out/` | `t0/out_workspace/out/include/out/` | `put.hpp` (`ok() -> T*` / `error()` / bool conversion; separate `Put<void,E>` specialization), `traits.hpp`, `retryable.hpp`, `utils.hpp`, `detail/` (dependency of walk/heap/tag::context). |

Rules:

- One grab-authored shim header `src/vendor/l0/log/writer.hpp` satisfies
  `#include <log/writer.hpp>` across **all five trees** (tag/gen.hpp, walk,
  heap, web, out all log). The real symbols used are `logger::tag("…")` and
  variadic `logger::trace(…)` / `logger::error(…)`; the shim provides exactly
  those as no-ops (plus any further `logger::` symbols a full-tree
  `grep -rn 'logger::' src/vendor/l0` reveals at drop time — final shim
  surface recorded in VENDOR.md).
- **Known upstream defects patched at drop** (each patch listed per-file in
  `src/vendor/l0/VENDOR.md`, with a regression test in grab):
  1. `walk/diff.hpp` packs edge endpoints as `(hash(a) * 1'000'003) ^ hash(b)`
     for set membership — collisions silently merge distinct edges (verified:
     knots `(1,2)` and `(3,2262152)` collide). Patch to an equality-aware
     pair key (e.g. `unordered_map<Knot, unordered_set<Knot>>`).
  2. `walk/graft.hpp` remaps sub-graph ids internally but does not return the
     old→new mapping, and creates attachment edges as `E{}`. Patch the return
     type to include the remap and take an explicit attachment edge value
     (the TreeStore needs both).
- All vendor headers compile under grab's toolchain flags **without further
  edits**; any additional incompatibility is fixed by a shim or a recorded
  minimal patch in `VENDOR.md`.
- A single CMake `INTERFACE` target `grab_vendor_l0` adds `src/vendor/l0` to
  the include path (`SYSTEM` to silence vendor warnings); only `src/` targets
  may link it. clang-tidy excludes `src/vendor/`.
- `out::Put` is not converted wholesale: grab code that calls a vendor API
  returning `out::Put<T,E>` converts to `grab::Result<T>` at the call site via
  `src/core/vendor_adapt.hpp` (`grab::detail::from_put(...)` — non-void and
  void overloads, built on `ok()`/`error()`).
- Public grab ABI never depends on vendor types: public refs are plain structs
  (§3); vendor `IdLane`/`Idx`/`Web` appear only in `src/`.
- `docs/` is currently in `.gitignore`; a `!docs/superpowers/` exception is
  added so this spec, the plan, and ADRs are tracked (plan Task 0).

## 3. Phase 0 contracts — new public value types

All new headers follow existing style: `#pragma once`, `namespace grab`,
4-space indent, CamelCase enum values, `EnumTable` name tables with
`static_assert`ed counts where strings are exposed.

### 3.1 `include/grab/ids.hpp`

```cpp
namespace grab
{
    // 16-byte RFC 9562 UUID value (v7 for operations: time-ordered in JSONL).
    struct Uuid
    {
        std::array<std::uint8_t, 16> bytes{};
        [[nodiscard]] bool is_nil() const;
        [[nodiscard]] std::string to_string() const;   // 8-4-4-4-12 lowercase
        friend auto operator<=>( const Uuid&, const Uuid& ) = default;
    };

    struct OperationId    { Uuid value{}; friend auto operator<=>( const OperationId&, const OperationId& ) = default; };
    struct SubscriptionId { Uuid value{}; friend auto operator<=>( const SubscriptionId&, const SubscriptionId& ) = default; };

    // Identifies one attached runtime instance; bumps on runtime restart.
    struct RuntimeId { std::uint32_t value{}; friend auto operator<=>( const RuntimeId&, const RuntimeId& ) = default; };

    // Monotonic generation counters (bumped on restart/resync; never reused
    // within a session). Wrappers, not raw ints, so domains cannot mix.
    struct DisplayGeneration { std::uint32_t value{}; friend auto operator<=>( const DisplayGeneration&, const DisplayGeneration& ) = default; };
    struct TreeEpoch         { std::uint32_t value{}; friend auto operator<=>( const TreeEpoch&, const TreeEpoch& ) = default; };
    struct NodeGeneration    { std::uint32_t value{}; friend auto operator<=>( const NodeGeneration&, const NodeGeneration& ) = default; };
    struct CaptureGeneration { std::uint32_t value{}; friend auto operator<=>( const CaptureGeneration&, const CaptureGeneration& ) = default; };

    struct WindowRef
    {
        DisplayGeneration display_generation{};
        std::uint32_t     xid{};    // opaque native window token (canonical plan R4);
                                    // meaning is driver-internal, callers never interpret it
        friend auto operator<=>( const WindowRef&, const WindowRef& ) = default;
    };

    struct WidgetRef        // runtime-scoped per canonical NodeRef contract
    {
        RuntimeId      runtime{};
        std::uint32_t  tree{};
        TreeEpoch      epoch{};
        std::uint64_t  node{};
        NodeGeneration generation{};
        friend auto operator<=>( const WidgetRef&, const WidgetRef& ) = default;
    };

    struct FrameId
    {
        std::uint64_t value{};
        friend auto operator<=>( const FrameId&, const FrameId& ) = default;
    };

    struct FrameRef         // canonical plan R4: capture evidence identity
    {
        CaptureGeneration capture_generation{};
        FrameId           frame{};
        // the frame's coordinate space travels in Frame metadata (§3.11);
        // FrameRef alone is the storable/wire identity
        friend auto operator<=>( const FrameRef&, const FrameRef& ) = default;
    };
}
```

Generation of `OperationId`/`SubscriptionId` values lives in
`src/core/id_factory.{hpp,cpp}` using vendored `tag::timed( tag::FastRng& )`
(free function; there is no `tag::gen` namespace). v7 gives millisecond
ordering with random tails — same-millisecond ids are **not** ordered — so the
factory adds a monotonic guard: under a mutex, if a fresh id compares `<=` the
last issued id, bump the random tail until it compares greater (RFC 9562 §6.2
monotonic-random method; also handles clock rollback). The existing
`WindowRef {uint32_t id, bool valid}` (flagged by the 07-05 review) is
**replaced**; `bool valid` disappears — validity is a generation check plus
typed errors.

### 3.2 `include/grab/space.hpp`

```cpp
namespace grab
{
    struct CoordinateSpaceId
    {
        std::uint32_t value{};
        friend auto operator<=>( const CoordinateSpaceId&, const CoordinateSpaceId& ) = default;
    };

    enum class TransformTrust : std::uint8_t { Exact, Calibrated, Heuristic, Untrusted };

    struct SpacePoint { double x{}; double y{}; CoordinateSpaceId space{}; };
    struct SpaceRect  { double x{}; double y{}; double w{}; double h{}; CoordinateSpaceId space{}; };

    // Row-major 2x3 affine map: x' = xx*x + xy*y + tx ; y' = yx*x + yy*y + ty.
    // Identity by default. Covers scale + translation + rotation/shear;
    // window-local -> global REQUIRES the translation terms.
    struct Affine
    {
        double xx{ 1.0 }; double xy{ 0.0 }; double tx{ 0.0 };
        double yx{ 0.0 }; double yy{ 1.0 }; double ty{ 0.0 };
    };

    // One edge in the transform graph (src/core owns the graph; this is the
    // public record surfaced through Frame metadata, receipts, and doctor).
    struct TransformRecord
    {
        CoordinateSpaceId source{};
        CoordinateSpaceId destination{};
        Affine            map{};
        std::uint64_t     mapping_id{};        // 0 = none (libei/PipeWire pairing when set)
        std::uint32_t     generation{};        // must equal the graph's current
                                               // generation for the edge's space,
                                               // else the route is stale
        TransformTrust    trust{ TransformTrust::Untrusted };
    };
}
```

The transform graph itself is `src/core/space_graph.{hpp,cpp}`:
`web::Web<web::OneWay, TransformRecord>` with `walk::path` route composition
(requires a `web::Trait<grab::TransformRecord>` specialization whose
`weight()` returns `1.0f` — shortest-hop routing; trust, not weight, carries
quality). Composition multiplies affines in route order; composed trust is the
weakest edge. Staleness: `SpaceGraph::bump_generation(CoordinateSpaceId)`
invalidates every edge touching that space; `map()` re-checks each route
edge's `generation` and returns `ErrorCode::TopologyChanged` on mismatch;
no route at all returns `ErrorCode::RouteUnavailable`. The existing
`GeometryTrust` concept (`src/input/locator.hpp`) migrates onto
`TransformTrust`.

### 3.3 `include/grab/context.hpp`

```cpp
namespace grab
{
    struct Deadline
    {
        std::chrono::steady_clock::time_point at{ std::chrono::steady_clock::time_point::max() };

        [[nodiscard]] static Deadline after( std::chrono::nanoseconds budget );
        [[nodiscard]] static Deadline unbounded();
        [[nodiscard]] std::chrono::nanoseconds remaining() const;   // clamped at zero
        [[nodiscard]] bool expired() const;
    };

    // DiagnosticEntry lives in grab/trace.hpp (§3.4) so result.hpp can hold
    // Error::diagnostics without an include cycle through this header.

    // Bounded ring; oldest entries drop, drop count retained. Attached to
    // failures (Error::diagnostics) and receipts.
    class DiagnosticLog
    {
    public:
        explicit DiagnosticLog( std::size_t capacity = 256 );
        void note( std::string message );
        [[nodiscard]] std::vector<DiagnosticEntry> snapshot() const;   // oldest -> newest
        [[nodiscard]] std::size_t dropped() const;
    private:
        std::vector<DiagnosticEntry> ring_;
        std::size_t                  capacity_;
        std::size_t                  next_{};
        std::size_t                  dropped_{};
    };

    // One per public operation; the Deadline is computed AT the public
    // boundary from a caller-supplied duration (public options carry
    // durations, never absolute deadlines — otherwise budget burns before
    // invocation). Nested work derives, never resets.
    struct OperationContext
    {
        Deadline                   deadline{};
        std::stop_token            stop{};
        OperationId                operation{};
        std::optional<OperationId> causal_parent{};
        DiagnosticLog*             log{ nullptr };   // borrowed; may be null

        [[nodiscard]] Result<void> check() const;    // DeadlineExceeded / Cancelled
        void note( std::string message ) const;      // no-op when log == nullptr

        // Same budget/stop/log; new child operation id, this id as parent.
        [[nodiscard]] OperationContext derived() const;
    };
}
```

### 3.4 `include/grab/trace.hpp`

```cpp
namespace grab
{
    enum class RetryClass : std::uint8_t { Never, ResolveOnly, Idempotent, Compensated };
    enum class ErrorDisposition : std::uint8_t { RetrySame, FallbackNext, Fatal };
    enum class CommitStatus : std::uint8_t { FailedBeforeCommit, PossiblyCommitted, Committed, Verified };
    enum class NeutralizationOutcome : std::uint8_t { NotAttempted, NothingHeld, Released, Failed };

    struct DiagnosticEntry             // shared by Error, DiagnosticLog, Receipt
    {
        std::chrono::steady_clock::time_point at{};
        std::string                           message;
    };
}
```

`Receipt` — the full action receipt — also lives **here** (canonical plan
places it in `trace.hpp`); it is *declared* in Phase 0 and populated by the
Phase-1 action engine:

```cpp
    struct RouteAttempt
    {
        std::string      route;                 // descriptor name
        bool             selected{ false };
        ErrorCode        rejection{};           // meaningful when !selected
        std::string      detail;
    };

    struct BarrierOutcome
    {
        std::string      barrier;               // e.g. "focus_enters_target"
        bool             satisfied{ false };
        bool             timed_out{ false };
    };

    struct Receipt
    {
        OperationId                  operation{};
        std::string                  locator;               // canonical serialization
        std::uint64_t                snapshot_revision{};
        CommitStatus                 commit{ CommitStatus::FailedBeforeCommit };
        std::vector<RouteAttempt>    routes;
        std::vector<BarrierOutcome>  barriers;              // armed pre-commit
        std::vector<TransformRecord> transforms;            // spaces crossed
        std::vector<FrameRef>        frames;                // evidence frames
        bool                         forced{ false };
        bool                         fallback_used{ false };// semantic->physical, explicit only
        NeutralizationOutcome        neutralization{ NeutralizationOutcome::NotAttempted };
        RetryClass                   retry_class{ RetryClass::Never };
        std::uint32_t                resolve_retries{};
        std::vector<DiagnosticEntry> log;
    };
```

(Default `neutralization` is `NotAttempted` — a receipt can never report
success for a release that never ran.)

### 3.5 `include/grab/origin.hpp`

```cpp
namespace grab
{
    enum class EventOrigin : std::uint8_t { Physical, InjectedSelf, InjectedOther, Unknown };
}
```

Wired into the event envelope in Phase 1 (§7); the enum + name table land in
Phase 0 so wire/proto numbering freezes early.

### 3.6 `include/grab/process_ref.hpp`

```cpp
namespace grab
{
    struct BorrowedProcessId          // observation/correlation only
    {
        std::int64_t value{ -1 };
        friend auto operator<=>( const BorrowedProcessId&, const BorrowedProcessId& ) = default;
    };

    // The only handle that can signal. Non-copyable; owns a pidfd; retains
    // start identity so PID reuse is detected.
    class OwnedProcess
    {
    public:
        OwnedProcess( OwnedProcess&& ) noexcept;
        OwnedProcess& operator=( OwnedProcess&& ) noexcept;
        OwnedProcess( const OwnedProcess& )            = delete;
        OwnedProcess& operator=( const OwnedProcess& ) = delete;
        ~OwnedProcess();

        // Adopt a DIRECT CHILD this process just created (fork/posix_spawn
        // return value, not yet reaped). Verifies /proc/<pid>/stat ppid ==
        // getpid() before capturing pidfd + starttime (field 22) as the start
        // token; any other pid — including a live unrelated one — returns
        // OwnershipRequired. There is deliberately NO adoption path for
        // arbitrary observed pids (canonical plan §7).
        [[nodiscard]] static Result<OwnedProcess> adopt_child( std::int64_t pid );

        [[nodiscard]] BorrowedProcessId id() const;
        [[nodiscard]] bool alive() const;
        [[nodiscard]] Result<void> terminate( std::chrono::nanoseconds grace );  // SIGTERM, wait, SIGKILL via pidfd; reaps
    private:
        int           pidfd_{ -1 };
        std::int64_t  pid_{ -1 };
        std::uint64_t start_token_{};
    };
}
```

Residual race note: between `fork()` and `adopt_child()`, the child may exit;
`pidfd_open` then fails and adoption returns `OwnershipRequired` — safe
(fail-closed). Callers must not `waitpid` the child before adopting.

Platform floor: `pidfd_open` (Linux 5.3), `pidfd_send_signal` (5.1),
`waitid(P_PIDFD, …)` (5.4), glibc wrappers since 2.36 — grab's process-
ownership feature requires **Linux ≥ 5.4 / glibc ≥ 2.36**; on older systems
`adopt_child` returns `CapabilityUnavailable` (compile-time fallback via
direct `syscall(2)` is allowed but the floor is documented in README).

The existing `include/grab/pid.hpp` migrates: usages that only correlate
become `BorrowedProcessId`; supervisor/child PIDs (managed workspaces,
`src/screen/virtual_display.cpp`'s Xvfb child — currently raw `kill()` at
lines 420/450) become `OwnedProcess`.

### 3.7 `include/grab/result.hpp` extensions

Appended `ErrorCategory` values (numbering continues; high byte of
`ErrorCode` = category index + 1, existing scheme):

```cpp
    enum class ErrorCategory : std::uint8_t
    {
        Environment, Permission, Target, Protocol, Usage, InternalFault,
        Action,      // ErrorCode high byte 0x07 — action transaction outcomes
        Stream,      // ErrorCode high byte 0x08 — subscription/queue outcomes
    };
```

New `ErrorCode` entries — **26** of them (existing 19 keep their values;
total 45). `category_of` gains explicit `0x07 -> Action` and
`0x08 -> Stream` mappings (today's implementation folds unknown high bytes
into `InternalFault`).

```cpp
        // Environment (1)
        TopologyChanged        = 0X01'03,
        // Permission / authority (3)
        LeaseClosed            = 0X02'02,
        LeaseRevoked           = 0X02'03,
        OwnershipRequired      = 0X02'04,
        // Target / identity / data (10)
        StaleNode              = 0X03'03,
        TreeResynced           = 0X03'04,
        RuntimeRestarted       = 0X03'05,
        TargetDetached         = 0X03'06,
        NoMatch                = 0X03'07,
        AmbiguousMatch         = 0X03'08,
        PropertyAbsent         = 0X03'09,
        PropertyUnsupported    = 0X03'0A,
        PropertyUncached       = 0X03'0B,
        PropertyBackendFailed  = 0X03'0C,
        // Usage (2)
        DeadlineExceeded       = 0X05'07,
        Cancelled              = 0X05'08,
        // Action (6)
        NotActionable          = 0X07'00,
        Occluded               = 0X07'01,
        RouteUnavailable       = 0X07'02,
        PossiblyCommitted      = 0X07'03,
        VerificationFailed     = 0X07'04,
        NeutralizationFailed   = 0X07'05,
        // Stream (4)
        QueueGap               = 0X08'00,
        ResyncRequired         = 0X08'01,
        SubscriptionGone       = 0X08'02,
        Overflowed             = 0X08'03,
```

`errorCodeCount` becomes **45**; name/category tables and their
`static_assert`s update in the same change. `struct Error` (currently a
five-field aggregate at `result.hpp:131-138`; tail-appending defaulted
members preserves existing designated-initializer construction) gains:

```cpp
        ErrorDisposition             disposition{ ErrorDisposition::Fatal };
        std::vector<DiagnosticEntry> diagnostics{};   // filled from OperationContext on failure
```

Message text stays human-only; tests assert on codes, never prose.

### 3.8 Options and knobs (verify + finish; corrected inventory)

Ground truth (rev2): three of the six canonical offenders are **already
conforming** after the in-flight reorg — event polling
(`src/event/platform_factory.hpp:14-25`), state snapshot interval
(`src/event/state_source.hpp:25-32`), and event-bus queue depth
(`include/grab/event_bus.hpp:71,88-89`) are named, defaulted, overridable.
`include/grab/drag.hpp` conforms with members `interpolation_steps` /
`step_dwell` (named-constant defaults; **no easing member exists or is
added — YAGNI**).

Remaining Phase-0 work: migrate the last two literals —

| Current literal | Destination |
|---|---|
| notification timeout (`src/notify/notifier.cpp`) | `NotifyOptions{ static constexpr default_timeout; timeout }` |
| service polling (`src/transport/service.cpp`) | `ServiceOptions{ static constexpr default_poll_interval; poll_interval }` |

— plus one pinning test per already-conforming knob (assert the default's
value so wire-visible timing stays deliberate). Layered provenance
(defaults < policy < project < env < CLI < per-request) and
freeze-after-composition are Phase-1 spine work.

### 3.9 Session vs Workspace rename (corrected to current tree)

`include/grab/session.hpp` today holds the live `Session` (+ `Impl`,
`SessionOptions`) **and** the managed-environment vocabulary: `SessionMode`,
`SessionState`, `SessionGeometry`, `SessionDesc` (+ their `EnumTable`s). There
is no separate managed class in the header (managed logic lives under
`src/session/`). The split:

- `include/grab/workspace.hpp` receives `WorkspaceMode`, `WorkspaceState`,
  `WorkspaceGeometry`, `WorkspaceDesc` (moved definitions, unchanged shapes
  and wire names), plus whatever managed entry points `src/session/` exposes
  publicly (inventory at execution).
- `session.hpp` keeps the live Session and deprecated aliases with the
  **correct C++ attribute placement**:

```cpp
    using SessionMode     [[deprecated( "use WorkspaceMode" )]]     = WorkspaceMode;
    using SessionState    [[deprecated( "use WorkspaceState" )]]    = WorkspaceState;
    using SessionGeometry [[deprecated( "use WorkspaceGeometry" )]] = WorkspaceGeometry;
    using SessionDesc     [[deprecated( "use WorkspaceDesc" )]]     = WorkspaceDesc;
```

### 3.10 CI invariant checks

`tests/scripts/check_invariants.sh` (registered as ctest
`grab_invariant_checks`), failing on:

- vendor namespaces (`tag::|web::|walk::|heap::|out::`) in `include/grab/`;
- `src/`-tree includes in `include/grab/` (any of
  `#include "(input|event|screen|core|session|transport|codec|platform|kernel|spi|drivers|frontends|compat)/`);
- platform headers in `include/grab/` (`#include <xcb/`, `<X11/`,
  `<wayland-`);
- raw sleeps (`sleep_for|usleep(|nanosleep(`) under `src/` outside
  `src/vendor/` and the allowlist `tests/scripts/sleep_allowlist.txt`
  (**relative** paths, matched against normalized relative paths; the list
  may only shrink);
- `kill(`/`killpg(` under `src/` outside `src/vendor/` and
  `src/core/process_ref.cpp` — which requires §3.6's `virtual_display.cpp`
  migration to land first (same phase, ordered before this check turns on);
- platform headers under `src/kernel/` (activates when that dir appears).

### 3.11 Remaining Phase-0 canonical obligations (rev2 additions)

Rev1 under-scoped Phase 0; the following canonical WP0 items are Phase 0:

- **ADRs** under `docs/superpowers/adr/`: `0001-lifetime-and-identity.md`
  (locator/ref/record split, generations), `0002-no-merged-graph.md`,
  `0003-registry-plan-coherence-and-wayland-lease.md`,
  `0004-descriptor-tables-and-extensions.md`,
  `0005-process-ownership.md`. One page each; decision + consequences.
- **`include/grab/frame.hpp`** — type-level `Frame` metadata (canonical
  WP0.4): `Frame{ FrameRef ref; CoordinateSpaceId space; SpaceRect
  content_rect; double scale; std::chrono::steady_clock::time_point
  captured_at; TransformRecord to_global; }` alongside the existing `Image`
  (capture code adopts it in Phase 1; the type freezes now).
- **`src/core/cleanup_registry.{hpp,cpp}`** — session-keyed undo registry
  (canonical WP0.5): `register_undo(name, fn)`, LIFO `run_all()` with
  per-entry error capture; every host mutation (added keymap entries, created
  virtual devices, spawned children) registers here.
- **`src/core/pressed_set.hpp`** — pure seat-state type (canonical WP0
  neutralization gate): tracks pressed keys/buttons; `neutralize()` returns
  the exact release sequence; unit-testable without a display.
- **Queue-gap semantics** (canonical WP0 gate): `QueueOptions{ capacity,
  overflow: Coalesce|NeverDrop }` + a gap-marker value type
  `GapMarker{ last_source_sequence, dropped }` with unit tests proving
  bounded-queue overflow yields a `GapMarker`, never silent loss.
- **Two-Sessions test** (canonical WP0 exit gate): two `grab::Session`
  instances in one process, independently opened/closed, no shared-state
  interference (ring-1).

## 4. Phase 1 object model (kernel)

### 4.1 Records and snapshots

```cpp
// include/grab/ui.hpp
namespace grab
{
    struct RoleId       { std::uint32_t value{}; };   // closed core table + namespaced extensions (>= 0x8000'0000)
    struct RelationId   { std::uint32_t value{}; };   // closed core table (< 32 core relations) + extensions
    struct PropertyId   { std::uint32_t value{}; };
    struct NodeId       { std::uint64_t value{}; };   // unique within (tree, epoch)

    enum class NodeState : std::uint32_t             // bitmask
    { Active = 1u << 0, Focused = 1u << 1, Visible = 1u << 2, Selected = 1u << 3,
      Enabled = 1u << 4, Editable = 1u << 5, Expanded = 1u << 6, Busy = 1u << 7 };

    using PropertyValue = std::variant<std::monostate, bool, std::int64_t, double,
                                       std::string, SpaceRect>;

    // Typed property read — preserves the taxonomy (§3.7) instead of erasing
    // it into an optional.
    struct PropertyRead
    {
        enum class State : std::uint8_t { Present, Absent, Unsupported, Uncached, BackendFailed };
        State         state{ State::Absent };
        PropertyValue value{};                 // meaningful only when Present
    };

    struct UiNodeRecord
    {
        NodeId         id{};
        NodeGeneration generation{};
        RoleId         role{};
        std::uint32_t  states{};                     // NodeState mask
        [[nodiscard]] PropertyRead              property( PropertyId ) const;
        [[nodiscard]] std::span<const PropertyId> property_ids() const;
        // …provenance accessor (source runtime + revision)
    };

    // Immutable, revisioned, per-runtime snapshot (canonical §1 storage rule).
    struct UiSnapshot
    {
        RuntimeId      runtime{};
        std::uint32_t  tree{};
        TreeEpoch      epoch{};
        std::uint64_t  revision{};
        bool           complete{ true };             // projection-on-ingestion may be partial
        [[nodiscard]] const UiNodeRecord* node( NodeId ) const;      // nullptr = not in snapshot
        [[nodiscard]] std::span<const NodeId> roots() const;
        [[nodiscard]] std::span<const NodeId> related( NodeId, RelationId ) const;          // forward
        [[nodiscard]] std::span<const NodeId> related_reverse( NodeId, RelationId ) const;  // labelled_by etc.
    };
}
```

Internal storage (`src/kernel/graph/tree_store.{hpp,cpp}`): per-snapshot
parallel arrays indexed by `tag::Idx<NodeTag>`. **Relations**: the vendored
`web::Web` keys one edge per (source, target) pair and rejects parallel
edges (verified `web/web.hpp:73-86`), so a single `Web<OneWay, RelationId>`
cannot hold `contains` + `active_child` between the same nodes. Core relations
(< 32) are therefore stored as `web::Web<web::OneWay, RelationSet>` where
`RelationSet` is a bitmask of relation kinds carried on the single (source,
target) edge; reverse lookups (`related_reverse`, i.e. `labelled_by`) come from
the `OneWay` dual adjacency map. Extension relations (≥ 32, namespaced) that
need parallel edges between the same pair use a side multimap keyed on
(source, target, RelationId). Node property arrays live beside the topology
(the `web/syntax/tree` parallel-array pattern, reimplemented with AccessKit's
compact layout — uint8 property index into a dense variant vector). The
`TreeStore` validates every applied update (unknown parent, duplicate id,
cycle → typed rejection with provenance, never a throw); generation deltas
between the two buffered snapshots are computed with the **patched**
`walk::diff` (equality-keyed endpoints, not the collision-prone hash fold —
VENDOR.md patch); cross-runtime grafts use the **patched** `walk::graft`
(returns the fresh-id remap + explicit attachment relation). Events derive from
the old/new diff and publish **after** the store lock releases (QueuedEvents
discipline).

### 4.2 Runtime-scoped reference identity (canonical NodeRef)

The public generation-checked handle for a live node is `WidgetRef` (§3.1);
`FrameRef` is the capture-evidence identity. Both are runtime-scoped: a
`WidgetRef` names `{tree, epoch, node, generation}` and is only meaningful
against the runtime that issued it, resolved through a `UiTreeView` that
returns typed staleness. Raw native ids (XID inside `WindowRef`, AT-SPI path,
CDP backend-node id) stay **driver-private aliases** — `WindowRef::xid` is an
opaque native token surfaced only because canonical plan R4 names it; callers
never interpret it, and nothing else in the public API exposes a backend id.
`NodeRef` in the canonical vocabulary maps to `WidgetRef` here (grab keeps the
domain-specific name); a future generic `NodeRef` variant over
Widget/Window/Frame refs is a Phase-1 convenience, not a Phase-0 type.

### 4.3 Locator / Match / refs

```cpp
// include/grab/locator.hpp
namespace grab
{
    class Locator          // immutable, hashable, serializable plan; no handle
    {
    public:
        [[nodiscard]] std::string to_string() const;   // canonical serialization
        friend bool operator==( const Locator&, const Locator& );
    };

    enum class Cardinality     : std::uint8_t { ExactlyOne, First, All };
    enum class BoundaryPolicy  : std::uint8_t { SameTree, SameProcess, CrossEmbeds };
    enum class ConsistencyMode : std::uint8_t { Live, Revisioned, Pinned };

    struct Match
    {
        WidgetRef        ref{};
        ConsistencyMode  mode{};
        std::uint64_t    snapshot_revision{};
        // + evidence: matched predicates, visual MatchEvidence, provider provenance
    };
}
```

Resolution runs `walk::sweep`/`delve` visitors over the CSR snapshot behind an
injected `TreeNav` seam (`src/kernel/query/`); AT-SPI `Collection` pushdown +
exact residual evaluation are the plan-lowering hook. Compilation carries a
complexity budget (`LocatorLimits{ max_nodes = 8192 }`) and caret-context
syntax errors. `exactly_one()` fails on both zero and ambiguity.

### 4.4 Surfaces and TargetRegistry

```cpp
// include/grab/presentation.hpp
namespace grab
{
    struct SurfaceId { std::uint64_t value{}; };

    struct SurfaceRecord
    {
        SurfaceId         id{};
        DisplayGeneration generation{};
        CoordinateSpaceId space{};
        SpaceRect         bounds{};       // in parent/output space
        // capture/hit-test route availability exposed as facets
    };
}
```

`TargetRegistry` (`src/kernel/graph/target_registry.{hpp,cpp}`) holds durable
application/window-grade targets with alias edges `{authority, native id,
confidence, validity}`. X11 is its first writer (EWMH windows → targets +
surfaces); AT-SPI adds semantic aliases only via exact bridges (a window id
inferred from `_NET_WM_PID`-style heuristics is candidate evidence, never a
fuse).

## 5. Phase 1 runtime/driver SPI (internal)

```cpp
// src/spi/runtime.hpp
namespace grab::spi
{
    struct ProbeReport { bool usable{}; ErrorCode reason{}; std::string detail; };

    class Runtime
    {
    public:
        virtual ~Runtime() = default;
        [[nodiscard]] virtual std::string_view name() const = 0;
        [[nodiscard]] virtual std::uint32_t    generation() const = 0;
        virtual Result<void> start( const OperationContext& ) = 0;
        virtual Result<void> stop() = 0;
        [[nodiscard]] virtual TreeSource*     tree_source()     { return nullptr; }  // optional facets
        [[nodiscard]] virtual TopologySource* topology_source() { return nullptr; }
        [[nodiscard]] virtual EventSource*    event_source()    { return nullptr; }
        [[nodiscard]] virtual std::span<const RouteDescriptor> routes() const = 0;
    };
}
```

- **Probe by exercising**: the factory `probe()` performs one real operation
  (capture a frame / receive an event / move-and-verify pointer). Rejection
  reasons are sticky and surfaced verbatim by resolver errors and `grab doctor`.
- **Plan coherence** (R1): when the resolver composes routes from different
  providers into one operation, it requires same-runtime or an explicit
  registered bridge (shared coordinate mapping + identity edge), else rejects
  with `RouteUnavailable` + reason. Scalar quality ints → `RouteDescriptor{
  fidelity, latency_class, constraints }` + caller policy.
- **Demand-driven activation**: `EventSource::enable(spec)/disable(spec)` driven
  by EventBus subscription refcounts (XI2 masks, evdev readers, AT-SPI
  RegisterEvent, XDamage lifetime).
- The existing X11 code is **wrapped** as one `X11Runtime` from its shared
  connection; no behavior rewrite in the wrapping step.

## 6. Phase 1 action engine

```cpp
// include/grab/interaction.hpp
namespace grab
{
    enum class RoutePolicy : std::uint8_t { PreferSemantic, SemanticOnly, PhysicalOnly };

    struct ActionOptions
    {
        std::chrono::nanoseconds deadline{ std::chrono::seconds{ 5 } };  // DURATION, not absolute — converted at the boundary (F21)
        Cardinality              cardinality{ Cardinality::ExactlyOne };
        RoutePolicy              routing{ RoutePolicy::PreferSemantic };
        RetryClass               retry{ RetryClass::ResolveOnly };
        bool                     force{ false };            // bypass preconditions; recorded in receipt
    };
}
```

`ActionOptions` carries a **duration**; `Session::perform` converts it to an
absolute `Deadline` and builds the `OperationContext` once, at the public
boundary. The full `Receipt` schema lives in `trace.hpp` (§3.4). Pipeline
(single impl in `src/kernel/action/transaction.cpp`): resolve (fresh snapshot)
→ prove cardinality → refresh transform → actionability predicates (event-woken,
bounded backoff, deadline-budgeted; occlusion verifiable via grab's own capture)
→ reserve seat lane → arm barriers → commit once → settle (watch own observation
streams, bounded) → verify → receipt. Held-input neutralization runs on **every**
terminal path; its outcome is the `Receipt::neutralization`
`NeutralizationOutcome` field (§3.4), which defaults to `NotAttempted` — a
receipt can never report success for a release that never ran (F12). The wait engine
(`src/kernel/action/wait_engine.{hpp,cpp}`) is the only wait primitive in the
tree; the CI sleep ban (§3.10) enforces migration off ad-hoc sleeps.

## 7. Phase 1 event-model changes

- **Envelope gains** (additive proto fields; v1 decode tolerates absence):
  `EventOrigin origin`; `std::uint64_t source_sequence` (per-source scope);
  an optional **`EventSubject`** distinct from the producer resource; optional
  `OperationId cause`; optional before/after snapshot revisions.
- **`EventSubject` is snapshot-scoped, never a live ref** (F13). Stored/wire
  events must be meaningful after the fact, so the subject is a value record,
  not a `WidgetRef`/`WindowRef`:

  ```cpp
  struct EventSubject
  {
      RuntimeId     runtime{};
      std::uint32_t tree{};
      TreeEpoch     epoch{};
      NodeId        node{};
      std::uint64_t revision{};   // snapshot revision the subject was observed at
  };
  ```
- **Descriptor rows gain**: replay policy (`None | CurrentSet`), coalescing
  class (`Coalesce | NeverDrop`), subject schema. `ListEventTypes` extends
  accordingly.
- **New graph events**: `node.added`, `node.removed`, `node.changed`,
  `relation.changed`, `active_child.changed`, `surface.changed` — payloads
  carry `EventSubject` + revisions, never refs.
- `browser.*` / `BrowserTab` payloads move to `src/compat/eventgrab_v1/` as
  projections derived from `active_child.changed` + node properties; the title
  classifier becomes an evidence adapter feeding node properties with
  `TransformTrust::Heuristic`-grade confidence, never identity.
- **Subscription objects**: server-issued `SubscriptionId`, declarative
  {event set, scope}; per-type replay on subscribe; bounded queues (byte + item
  budgets); overflow emits a `QueueGap` marker carrying the last source sequence
  and (for tree scopes) forces a resnapshot.

## 8. Phase 2/3 boundary contracts (specified here, implemented later)

- **SessionLease** (Phase 2): opaque handle for granted portal authority;
  resolver output type on Wayland; capture/input routes attach to it; closure
  invalidates routes + refs deterministically. Restore-token vault: delete-on-use,
  transient vs persistent, re-persist after every Start.
- **Agent surface** (Phase 3): snapshot/act tool set generated from the
  CommandDescriptor table; refs epoch-scoped with re-snapshot recovery errors;
  responses = executed operation + refreshed observation + ambient notes.
- Neither phase may add `include/grab/` types contradicting §1 invariants; each
  gets its own spec + plan.

## 9. Public/source layout target (end of Phase 1)

Per canonical plan §12. Phase 0 adds public headers `ids.hpp`, `space.hpp`,
`context.hpp`, `trace.hpp`, `origin.hpp`, `process_ref.hpp`, `workspace.hpp`
and `src/vendor/l0/`. Phase 1 adds `ui.hpp`, `role.hpp`, `relation.hpp`,
`facet.hpp`, `locator.hpp`, `query.hpp`, `presentation.hpp`, `interaction.hpp`,
`watch.hpp`, `window_match.hpp`, and `src/kernel/{graph,identity,query,action,
event,routing}`, `src/spi/`, `src/drivers/{desktop,semantic,device}/`,
`src/compat/eventgrab_v1/`. Existing `screen.hpp`/`input.hpp` remain as facades
delegating to session verbs. Dependency rules are the §3.10 CI checks: public
headers never include `src/` or platform headers; kernel never includes
platform/browser headers; drivers depend on SPI + public value types, never on
each other.

## 10. Testing requirements

- Every Phase-0 type: ring-1 unit tests in `tests/core/` (added to
  `grab_core_tests`), per plan Tasks 2–11.
- Fake runtime: `tests/fake/` — scriptable `TreeSource`/`EventSource` (inject
  snapshots, deltas, restarts, permission prompts, queue overflow, partial
  commit). Seeded in plan Task 11; promoted to a full `FakeRuntime` in P1.1.
- **Phase-0 exit gates** (plan Task 11 + §3.11): generation staleness
  (`StaleNode`), epoch resync (`TreeResynced`), deadline budget consumed across
  nested contexts, `DiagnosticLog` bounding + drop count, `OwnedProcess`
  terminates only a verified direct child (non-child → `OwnershipRequired`),
  affine transform composition incl. translation + stale-route typing,
  invariant-check script green, existing suite green.
- **Phase-1 exit gate**: the canonical completion test at X11 scope — one
  resolved generic node supports capture + pointer + keyboard + semantic invoke
  + watch through one call path (CLI and daemon both via the P1.10 client);
  every mutating verb returns a `Receipt`; daemon restart replays subscriptions;
  two Sessions in one process; admission control rejects honestly under load;
  v1 wire compatibility green.

## 11. Non-goals (this spec)

Wayland/portal implementation, agent tool surface, Windows/macOS/CDP/BiDi
runtimes, the `grab::sel` string front end beyond the typed builder,
settings-provenance layering beyond naming the knobs, video/tile-differ rework
beyond the WP1.12 contract. Each is contracted at its boundary here (§8) and
specified in its own follow-up.

<!-- rev2 note: §4.2–§11 restored after the rev2 rewrite was truncated at §4.1
by a context compaction. Findings folded here: F13 (EventSubject snapshot-scoped),
F5 completion (RelationSet + extension multimap), F11 (runtime-scoped refs, xid
as opaque token), F12 (Receipt in trace.hpp, neutralization not defaulted-success),
F21 (ActionOptions carries duration). -->
