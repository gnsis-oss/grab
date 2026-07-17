# Purge Wave 4 — Observation Purge Implementation Plan (daemon swap + browser demotion)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> Parent plan: `docs/superpowers/plans/2026-07-16-legacy-purge.md` (Wave 4, "Observation purge", lines 200-217). Execution pipeline: Claude orchestrator drives Codex sol workers; Codex edits, orchestrator reviews/tests/commits (single-author rule). This plan is the bite-size TDD expansion of that Wave-4 table.

**Goal:** Make the canonical runtime stack the *only* live observation stack. Retire `src/event/` (5,486 LOC → 0): the daemon stops composing `PlatformFactory`/`SourceRegistry` and instead runs a **continuous observation pump** over the `Session`'s runtime event/tree sources; the browser vocabulary (`BrowserTab`/`BrowserTabSwitched`/`EventCategory::Browser`) leaves the core and is re-derived as a v1-wire projection in `src/compat/eventgrab_v1/`; the JSONL storage envelope is upgraded to carry the fields it currently drops; and the frozen, still-consumed legacy files (`atspi.cpp`, `evdev.cpp`, `window_x11.cpp` EWMH, `browser_bridge.cpp`, the classifier) are **relocated/re-homed, not blindly deleted**.

**Architecture:** Two pipelines run in parallel today. (1) Legacy: `Daemon::Impl::start_sources()` → `PlatformFactory::build()` → `registry_.start_all(reactor_, bus_)` (`src/service/daemon.cpp:397-406`) feeds the daemon's `bus_` (which storage drains, `daemon.cpp:471-473`). (2) Canonical: the daemon owns one `grab::Session` (`daemon.cpp:277,334`) whose `SessionCore` (`src/kernel/lifecycle/session_impl.cpp`) composes `X11Runtime`+`AtspiRuntime`+per-runtime `TreeStore`s and its **own** `EventBus` (`session_impl.hpp:155`), but that Session is used only for verb RPCs — its event sources are never pumped. This wave deletes pipeline (1) and makes pipeline (2) the live source, then unifies the two buses so storage/transport read the runtime output.

**Tech Stack:** C++23, clang++23/Ninja (`cmake --preset default`, never GCC/Make), GTest + Xvfb fixture (`tests/CMakeLists.txt:159-191`, `DISPLAY=:87`), gRPC/protobuf wire at `proto/eventgrab/v1/`, in-tree PNG codec. `-Werror -Wall -Wextra -Wshadow` + `clang-tidy` on every TU (`cmake/Warnings.cmake`, `cmake/Tidy.cmake`).

## Global Constraints

- Worktree `grab/.worktrees/wave4`, branch `purge/wave4-event`. Repo git identity only; no AI attribution; verify single author per task commit (`git log --format='%an <%ae> / %cn <%ce>'`).
- Build + `ctest --output-on-failure` green before every commit; **re-run the known socket/Xvfb flakes serially before judging** (`EventServiceCommands.CaptureFrameRoundTripsPngOverSocket`, `ClientSocketVerbs.VerbsRoundTripThroughTransportServer` — both are gRPC-deadline flakes under parallel load, pass with `ctest -j1 -R`).
- Legacy dirs are frozen (LOC ratchet, `tests/scripts/check_legacy_ratchet.sh`). Every ported target lands in `src/{kernel,spi,drivers,client,transport,service,cli,compat}`. A budgeted dir's line in `tests/scripts/legacy_budget.txt` may only shrink; **deleting a dir deletes its line in the same commit** (`src/platform=1243` was already removed in W4.1). `src/event=5486` shrinks each task and is deleted with the final `src/event` removal.
- Public headers: no `src/` includes, no vendor namespaces (`grab_invariant_checks` #2/#13 enforce, `check_invariants.sh:8-14`). Removing a public header (`active_kind_probe.hpp`) also removes its line from `tests/scripts/public_header_allowlist.txt` **in the same commit** (ratchet #2, `check_legacy_ratchet.sh:22-26`).
- Naming: CamelCase enum values, no `k` prefix, trailing `_` privates, `EnumTable` + `enum_table_has_count` static_asserts.
- Wire changes are additive/compat only; `eventgrab.v1` round-trip tests must stay green **through the compat projection** once browser leaves the core.
- **Do not touch `src/input/**` or `src/cli/**` semantics** while the parallel input-purge worktree is live; the only permitted `src/cli` touch in this wave is re-pointing the two live `WindowTracker`/EWMH consumers (Task 6) and must be coordinated (a mechanical include/callsite repoint, not verb work). Remove the temporary `src/compat/x11_relocation_shim/` (added in W4.1) once `src/input`/`src/event`/`tests/input` are repointed off `platform/x11/` (Task 7 or a coordinated follow-up).

## Verified seams (2026-07-17; re-verify lines before editing)

- **Daemon composition (the crux):** `src/service/daemon.cpp` — members `bus_` (:274), `reactor_` (:275), `registry_` = `grab::event::SourceRegistry` (:276), `session_` = `unique_ptr<grab::Session>` (:277), `sink_` = `JsonlSink` (:279); `start_sources()` composes `source_factory_() ? ... : PlatformFactory::build(SourceConfig{})` then `registry_.add(...)` + `registry_.start_all(reactor_, bus_)` (:397-408); `session_` created via `grab::Session::open(SessionOptions{})` and handed to `TransportServer::start(endpoint_, bus_, &registry_, session_.get())` (:331-343); storage drain subscribes `bus_` (:471-473); shutdown `registry_.stop_all()` (:608) + `session_.reset()` (:625). `DaemonOptions::source_factory` is the test seam (:272-273,291).
- **SessionCore:** `src/kernel/lifecycle/session_impl.hpp` — owns `bus_` (:155), per-runtime `bindings_` each with `std::unique_ptr<TreeStore> store` (:125-134,156), `owned_runtime_`/`atspi_runtime_` (:159-160), `pump_once(ctx)` drains **only tree sources' `next_update`** into stores (:119, impl drains `spi::TreeSource`), `bus()` (:88), `store_at`/`store_count` (:97-101). **There is no event-source pump.** RuntimeId note (:165-174): X11 mints `RuntimeId{1}`, AtSpi increments; separate stores prevent storage collision, **but bus-event `subject.runtime` stays ambiguous across runtime restarts — a session-level RuntimeId authority is deferred (Wave-1 "Task 7b").**
- **Runtime event-source SPI:** `src/spi/runtime.hpp:71-76` `event_source()` (returns `nullptr` by default; `X11Runtime` overrides at `x11_runtime.cpp:232-234`); `src/spi/event_source.hpp:20-48` — `EventSource::{enable,disable}(EventSpec)`, `wait_for_event(EventSpec, OperationContext, nanoseconds)` (blocks on the fd, honors deadline/cancellation).
- **X11EventSource:** `src/drivers/desktop/x11/x11_event_source.hpp:19-75` — `open(connection, root, InjectionLedger&)`, `set_sink(EventSink = std::function<void(grab::Event&&)>)`, demand-driven `enable/disable`, `wait_for_event`. `X11Runtime` owns it + `InjectionLedger` (`x11_runtime.hpp:118-119`), opens it in `start()` and wires `set_sink(pending_sink_)` (`x11_runtime.cpp:100-115,184-186`). Nothing loops `wait_for_event`.
- **Storage envelope drop:** `src/storage/jsonl_sink.cpp:334-354` `serialize_line` writes ONLY `{ts, type, category, data}` — drops `event.sequence` (event.hpp:162), `origin` (:166), `subject` (:167-168, `EventSubject{runtime,tree,epoch,node,revision}` at :150-157), `cause` (:169-170), `before_revision`/`after_revision` (:171-174).
- **Event vocabulary + envelope:** `include/grab/event.hpp` — `EventCategory::Browser=5U` (:24), `Count=7U` (:26); `EventKind::BrowserTabSwitched=500U` (:51); `struct BrowserTab` (:114-120); `Payload` variant entry (:143); generic graph kinds `NodeAdded=700..ActiveChildChanged=705` (:53-58); envelope `Event` (:159-175).
- **Browser blast radius (count-locked tables — removal is atomic):** descriptor row `event_descriptor.hpp:153-155` + category name `:217` + static_assert `:221-222`; payload fields `payload_fields.hpp:32-33,64-65` + Count `:38` + static_assert `:72-73`; proto `events.proto:10,41-42`; parity `proto_descriptor.hpp:62-63,103-104` + static_asserts `:165-230`; codec encode `codec.cpp:380-394` / decode `:658-686` / dispatch `:735-736,842-844`; storage `jsonl_sink.cpp:273-288`; synthesis `window_x11.cpp:70,392,422,462-474,865,883-895,924-934` (derives tab from **WM_CLASS + title + PID**, not active-child); classifier `browser_classifier.hpp:16-143` (consumers = `window_x11.cpp` + test only); bridge `browser_bridge.{hpp,cpp}` (constructed only by `platform_factory.cpp:147-158`; also carries `AppTabChanged`/`AppContextUpdate`). `window.created` replay row is `ReplayPolicy::None` at `event_descriptor.hpp:113-117` (parent plan: → `CurrentSet`).
- **Legacy composition to delete:** `PlatformFactory::build` order = input(evdev|xinput2) → window → a11y → browser → state (`platform_factory.cpp:80-166`); `SourceRegistry::start_all` degrade-skips failures, returns success (`source_registry.cpp:29-55`); `SourceRegistry::is_kind_active` implements `grab::ActiveKindProbe` (`source_registry.cpp:74-101`). `ActiveKindProbe` consumers to re-point: `transport/service.{cpp,hpp}`, `transport/server.{cpp,hpp}`, `client/loopback_transport.{cpp,hpp}`, `event_descriptor.hpp` (`git grep active_kind_probe`).
- **Still-live legacy consumers (block naive delete):** `AtspiRuntime` wraps `grab::event::AtspiMonitor` (`atspi_runtime.hpp:116`, `atspi_runtime.cpp:278,292`); the X11 driver's `workflow.cpp:368` and `cli/main.cpp:1747` construct `grab::event::WindowTracker`.

Dependency order: **T1 → T2 → {T3, T4} → T5 → T6 → T7 → T8**, with **T0 (RuntimeId authority)** a prerequisite for T8's `subject.runtime` correctness (may run in parallel with T1–T6). T3/T4 parallelizable. T7 (browser) requires T5 (real `active_child.changed` deltas) landed. Final cleanup task (delete `src/event/` + budget line) closes the wave.

---

## Legacy source disposition (RELOCATE vs DELETE vs SUPERSEDED)

| File (LOC) | Disposition | Destination / rationale (file:line evidence) |
|---|---|---|
| `xinput2.cpp` (635) + `.hpp` | **DELETE — SUPERSEDED** | XI2 setup + raw decode is line-for-line reimplemented (superset, adds injection provenance) in `x11_event_source.cpp` — e.g. `require_xi2` (`xinput2.cpp:125-152` ≈ `x11_event_source.cpp:130-157`), `append_decoded_event` (`xinput2.cpp:358-423` ≈ `x11_event_source.cpp:310-397`). Delete once the daemon no longer wires it via `PlatformFactory` (T2). |
| `evdev.cpp` (452) + `.hpp` | **RELOCATE** | `src/drivers/device/evdev/` — pure Linux `/dev/input`+epoll (`evdev.cpp:13-16`), **no** driver-side equivalent; reachable only when `evdev_device` configured. Port `EvdevMonitor` to `spi::EventSource` with demand-driven reader (T4). |
| `window_x11.cpp` (1005) + `.hpp` | **RE-HOME (not superseded)** | EWMH tracker is unique AND live: consumed by `workflow.cpp:368` and `cli/main.cpp:1747`. Its focus/title/create/destroy logic becomes real `X11TreeSource::next_update`/topology deltas (`window.*` + `active_child.changed`); re-point the two live consumers (T5). |
| `atspi.cpp` (995) + `.hpp` | **RELOCATE (not superseded)** | `src/drivers/semantic/atspi/` — the new `AtspiRuntime` **wraps** `grab::event::AtspiMonitor` (`atspi_runtime.cpp:278`). Relocate verbatim under the atspi driver; make `RegisterEvent` demand-driven (T3). Deleting it breaks the driver. |
| `state.cpp` (179) + `.hpp` | **RE-HOME** | TreeStore/EWMH `StateSnapshot` provider registered with the bus; no driver equivalent, used only by `StateSource` (`state_source.cpp:48-57`) (T6). |
| `state_source.cpp` (309) + `.hpp` | **RE-HOME** | snapshot-provider registration on the bus; fix `window.created` replay `None`→`CurrentSet` (`event_descriptor.hpp:113-117`) (T6). |
| `browser_bridge.{cpp,hpp}` (640) | **RELOCATE** | `src/drivers/semantic/webextension/` — native-messaging bridge, no driver equivalent, off by default; also carries `AppTabChanged`/`AppContextUpdate` integration kinds (decide their fate here) (T7). |
| `browser_classifier.hpp` (145) | **DEMOTE + RELOCATE** | low-confidence **evidence adapter** feeding `TargetRegistry` candidate aliases (never identity); relocate under `src/compat/eventgrab_v1/` or the webextension driver (T7). |
| `platform_factory.{cpp,hpp}` (205) | **DELETE** | legacy DI; replaced by the observation pump (T2). |
| `source_registry.{cpp,hpp}` (185) | **DELETE** | legacy lifecycle + `ActiveKindProbe` impl; re-point `ActiveKindProbe` consumers to a runtime/descriptor source (T2). |
| `monitor_source.hpp`, `source.hpp` (180) | **DELETE** | legacy `EventSource` contract + adapter, distinct from `spi::EventSource`; die with the swap (T2). |

---

### Task 0 (prerequisite for T8): session-level RuntimeId authority

**Files:**
- Modify: `src/kernel/lifecycle/session_impl.{hpp,cpp}` (replace the per-runtime `next_runtime_id_` seed (:174) with a monotonic session-scoped allocator handed to each runtime at `attach()`/compose time; `X11Runtime`/`AtspiRuntime` take an injected `RuntimeId` instead of minting `RuntimeId{1}` from their own generation)
- Test: `tests/kernel/lifecycle/test_session_core.cpp`

**Interfaces:**
- Produces: distinct, stable `subject.runtime` per attached runtime across restarts, so the storage envelope (T8) can disambiguate multi-runtime output. Closes the deferral recorded at `session_impl.hpp:165-174`.

- [ ] **Step 1: Failing test** — attach two FakeRuntimes; assert `store_at(0)` and `store_at(1)` scopes have distinct `RuntimeId`, and that a simulated runtime restart re-attaches with the **same** id (not a colliding `{1}`).
- [ ] **Step 2-3:** implement the allocator; thread the id into `X11Runtime`/`AtspiRuntime` construction (`session_impl.cpp:134,225`).
- [ ] **Step 4:** full suite green → commit — `refactor(session): session-scoped RuntimeId authority replaces per-runtime seed (W4.0)`

> **Why a prerequisite:** per-runtime `TreeStore`s already prevent *storage* collisions, but T8 writes `subject.runtime` into the JSONL envelope; without a session-level authority the daemon's multi-runtime path (X11 + AtSpi + evdev) records ambiguous runtime ids. If T8 must ship before T0, gate it: write `subject.runtime` only for the single primary runtime and record a diagnostic, exactly as Wave-1 gated AT-SPI attach.

### Task 1: Continuous observation pump in SessionCore

**Files:**
- Create: `src/kernel/lifecycle/observation_pump.{hpp,cpp}` (class `grab::kernel::lifecycle::ObservationPump`: owns a `std::jthread` per attached runtime with an `event_source()`; loops `wait_for_event(spec, ctx, deadline)`; the runtime's `set_sink` publishes each `grab::Event` onto `SessionCore::bus()`; also periodically calls `SessionCore::pump_once(ctx)` to drain tree deltas; honors `std::stop_token`)
- Modify: `src/kernel/lifecycle/session_impl.{hpp,cpp}` (add `Result<void> start_observation(const OperationContext&)` / `stop_observation()`; wire `bus().set_demand_callback` → `event_source().enable/disable` so XI2 masks track subscriber count — the audit's missing demand propagation), `CMakeLists.txt` (new sources into `grab_kernel` or `grab_core`)
- Test: `tests/kernel/lifecycle/test_observation_pump.cpp` (FakeRuntime whose `event_source()` yields a scripted event on `wait_for_event`)

**Interfaces:**
- Produces: a live pump that turns runtime `wait_for_event` output into `bus_` publications with `EventOrigin` intact; demand callback toggles masks (`x11_event_source.cpp` `enable/disable` refcounts gain their consumer).
- Consumed by: T2 (daemon), and by any `grab::Session` that wants live observation (currently only verbs run).

- [ ] **Step 1: Failing test** — FakeRuntime event source scripted to emit one `KeyDown` on first `wait_for_event`; `ObservationPump` started against a SessionCore; subscribe on `{KeyDown}`; assert the event arrives on the bus within a bounded wait and carries the source's `origin`.
- [ ] **Step 2:** run → fails (no header). **Step 3:** implement pump + demand wiring. **Step 4:** add a test that subscribing/unsubscribing flips a FakeRuntime `enable/disable` counter (demand propagation).
- [ ] **Step 5:** full suite green (incl. Xvfb `test_x11_event_source` still green) → commit — `feat(session): continuous observation pump drives runtime event sources onto the bus with demand-driven masks (W4.1)`

### Task 2: Daemon observation swap — retire PlatformFactory/SourceRegistry (THE CRUX)

**Files:**
- Modify: `src/service/daemon.cpp` (`start_sources()` (:353-409) no longer builds `PlatformFactory`/`registry_`; instead starts the Session's `ObservationPump`; **bus unification** — make storage + transport read the Session's bus: either construct the daemon's `session_` first and point `sink_`/`TransportServer` at `session_->core().bus()`, or inject the daemon's `bus_` into `SessionCore::open` as the shared bus. Remove `registry_` (:276), `source_factory_` (:272), `registry_.stop_all()` (:608)), `src/service/daemon.hpp`/`DaemonOptions` (drop `source_factory`; add a test seam for injecting a FakeRuntime-backed Session), `CMakeLists.txt` (`grab_service` drops `grab_event` link once nothing in it includes `event/`)
- Modify (re-point `ActiveKindProbe`): `src/transport/service.{cpp,hpp}`, `src/transport/server.{cpp,hpp}`, `src/client/loopback_transport.{cpp,hpp}`, `include/grab/event_descriptor.hpp` (source "is this kind active" from the runtime's advertised `routes()`/descriptor table instead of `SourceRegistry::is_kind_active`)
- **Delete:** `src/event/platform_factory.{cpp,hpp}`, `src/event/source_registry.{cpp,hpp}`, `src/event/monitor_source.hpp`, `src/event/source.hpp`, `src/event/xinput2.{cpp,hpp}` (SUPERSEDED); remove them from `CMakeLists.txt` `grab_event` (:132-142); lower `src/event` budget line accordingly; delete `tests/event/test_platform_factory.cpp`, `test_source_registry.cpp`, `test_monitor_source.cpp`, `test_xinput2.cpp` (`tests/CMakeLists.txt:48-50,57`)
- Test: `tests/service/test_daemon.cpp` (swap the `source_factory` seam for a FakeRuntime Session; assert an injected runtime event reaches storage); keep `test_service.cpp:639-640` list-event-types green via the re-pointed probe

**Interfaces:**
- Produces: a daemon whose only observation source is the canonical runtime stack; `ListEventTypes` answered from the descriptor/route table; storage drains runtime output.
- **Coupling to browser (say-how):** the legacy stack is the *only* current producer of `BrowserTabSwitched` (via `window_x11.cpp` synthesis + `browser_bridge`). After this swap, `BrowserTabSwitched` is **dormant** — the kind still exists in the vocabulary but nothing emits it — until T7 lands the compat projection. That is acceptable and intended: the swap removes the producer first; the core-vocabulary removal happens last (T7), once T5's `active_child.changed` deltas can feed the re-derivation.

- [ ] **Step 1: Failing test** — `test_daemon.cpp`: start a daemon with a FakeRuntime-backed Session that emits a `WindowFocusChanged`; assert the JSONL sink records it (proving the runtime→bus→storage path replaces `PlatformFactory`).
- [ ] **Step 2:** run → fails (daemon still on `PlatformFactory`). **Step 3:** implement the swap + bus unification + demand callback; re-point `ActiveKindProbe`. **Step 4:** delete the superseded files + tests; lower the budget line; green.
- [ ] **Step 5:** full suite green (Xvfb serially) → commit — `refactor(service): daemon observes through the runtime pump; delete PlatformFactory/SourceRegistry/xinput2 (W4.2)`

### Task 3: Relocate the AT-SPI monitor under the atspi driver, demand-drive it

**Files:**
- Relocate (verbatim `git mv`): `src/event/atspi.{cpp,hpp}` → `src/drivers/semantic/atspi/atspi_monitor.{cpp,hpp}` (keep `grab::event` namespace initially to minimize churn, or rename to `grab::drivers::semantic::atspi` and fix the two callsites `atspi_runtime.cpp:278,292`); repoint `#include "event/atspi.hpp"` in `atspi_runtime.hpp:? / .cpp`; move `src/event/atspi.cpp` out of `grab_event` into `grab_driver_atspi` (`CMakeLists.txt:210-213`)
- Move test: `tests/event/test_atspi.cpp` → `tests/drivers/atspi/`
- Modify: make `AtspiMonitor::RegisterEvent` demand-driven (enable/disable per subscriber) as the parent plan requires (`legacy-purge.md:208`)
- Test: `tests/drivers/atspi/test_atspi_runtime.cpp`

- [ ] Steps: failing relocation-compile test → `git mv` + include/CMake repoint → demand-drive `RegisterEvent` (small test: subscribe toggles registration) → suite green → commit — `refactor(driver): relocate the AT-SPI monitor under the atspi driver; demand-driven registration (W4.3)`

### Task 4: Relocate evdev under the device driver tree

**Files:**
- Relocate (`git mv`): `src/event/evdev.{cpp,hpp}` → `src/drivers/device/evdev/evdev_source.{cpp,hpp}`; new CMake target `grab_driver_evdev` (or fold into an existing device target); linked into the daemon's runtime composition only when a device is configured
- Modify: port `EvdevMonitor` from the legacy `EventSource` (reactor+bus) to `spi::EventSource` (`enable/disable` + `wait_for_event` reader thread) so the pump (T1) can drive it
- Move test: `tests/event/test_evdev.cpp` → `tests/drivers/device/evdev/`

- [ ] Steps: failing test (evdev source as `spi::EventSource` yields a decoded KeyDown from a scripted fd) → `git mv` + port + CMake → suite green → commit — `refactor(driver): relocate evdev as an spi::EventSource under drivers/device/evdev (W4.4)`

### Task 5: X11 window/EWMH deltas become real (window_x11 re-home)

**Files:**
- Modify: `src/drivers/desktop/x11/x11_tree_source.{cpp,hpp}` + `x11_topology_source.{cpp,hpp}` (implement real `next_update`: EWMH `_NET_ACTIVE_WINDOW`/`_NET_WM_NAME`/`_NET_WM_PID` tracking + Create/Destroy notify → `NodeAdded/NodeRemoved/NodeChanged` + `active_child.changed` graph deltas + `window.focus_changed/title_changed/created/closed`). Port the EWMH atom-interning + poll logic from `src/event/window_x11.cpp:147-186,520-616,834-955`.
- Re-point live consumers: `src/drivers/desktop/x11/workflow.cpp:368` (title-change screenshot route) and `src/cli/main.cpp:1747` — from `grab::event::WindowTracker` to the driver's tree/topology source or a thin shim (coordinate the `src/cli` touch with the input-purge worktree; mechanical only).
- **Delete:** `src/event/window_x11.{cpp,hpp}` once both consumers are re-pointed; drop from `grab_event` (`CMakeLists.txt:134`); lower budget; move/rewrite `tests/event/test_window_x11.cpp` under `tests/drivers/x11/`
- Test: `tests/drivers/x11/test_x11_tree_source.cpp` (Xvfb): map/focus/title a window → assert `NodeAdded`, `WindowFocusChanged`, `WindowTitleChanged`, and an `active_child.changed` on focus move.

**Interfaces:**
- Produces: `active_child.changed` graph deltas — **the input to T7's browser projection**. This task must land before T7.

- [ ] Steps: failing Xvfb tree-source test → port EWMH logic into the tree/topology source → re-point `workflow.cpp:368` + `cli/main.cpp:1747` → delete `window_x11.*` → suite green (Xvfb serially) → commit — `feat(driver): real X11 window/EWMH graph deltas; retire window_x11 tracker (W4.5)`

### Task 6: State snapshot provider re-home + replay-row fix

**Files:**
- Re-home: `src/event/state.{cpp,hpp}` + `src/event/state_source.{cpp,hpp}` → a TreeStore/EWMH `StateSnapshot` provider registered on the bus via `EventBus::register_snapshot_provider(EventKind::StateSnapshot, ...)` (kernel or a small `src/drivers/desktop/x11` provider). Preserve the once-at-start + periodic snapshot behavior (`state_source.cpp:90-124,210-270`).
- Modify: `include/grab/event_descriptor.hpp:113-117` — `WindowCreated` replay `ReplayPolicy::None` → `CurrentSet` (parent plan `legacy-purge.md:211`); update the descriptor test.
- **Delete:** `src/event/state*.{cpp,hpp}` from `grab_event`; lower budget; move `tests/event/test_state.cpp`, `test_state_source.cpp`, `test_monitor_source.cpp` (if not already deleted in T2)
- Test: `tests/kernel/...` snapshot-provider test + updated `tests/event/test_event_descriptor.cpp` (or its relocated home)

- [ ] Steps: failing test (subscriber with replay gets a `StateSnapshot` + a `window.created` CurrentSet replay) → implement provider + descriptor fix → suite green → commit — `feat(state): TreeStore-backed snapshot provider on the bus; window.created replay CurrentSet (W4.6)`

### Task 7: Browser demotion — `src/compat/eventgrab_v1/` + core vocabulary browser-free

**Files:**
- Create: `src/compat/eventgrab_v1/browser_projection.{hpp,cpp}` — derives the **v1** `browser.tab_switched` wire event from `active_child.changed` graph deltas (T5) + node properties (app/title/pid from the tree node), replacing `window_x11.cpp`'s WM_CLASS/title synthesis. This is a **v1-wire projection**, not a core kind.
- Relocate: `src/event/browser_classifier.hpp` → `src/compat/eventgrab_v1/browser_evidence.hpp`, demoted to a **low-confidence evidence adapter** feeding `TargetRegistry` candidate aliases (never identity — fixes invariants #1/#5/#8, `legacy-purge.md:210`).
- Relocate: `src/event/browser_bridge.{cpp,hpp}` → `src/drivers/semantic/webextension/` (contributes document/tab nodes + `active_child` relations); decide the `AppTabChanged`/`AppContextUpdate` integration kinds it also carries (keep as integration kinds, or project them through compat too).
- **Remove `BrowserTab` from the CORE vocabulary — one atomic commit** (count-locked static_asserts force it): `event.hpp` (drop `EventCategory::Browser` :24 + decrement `Count` :26; drop `EventKind::BrowserTabSwitched` :51; drop `struct BrowserTab` :114-120 + variant entry :143), `event_descriptor.hpp` (row :153-155, category name :217, keep static_asserts satisfied :221-222), `payload_fields.hpp` (drop `TabTitle`/`PrevTabTitle` :32-33,64-65 + decrement `Count` :38,:72-73). Move the v1 wire tag + encode/decode into `src/compat/eventgrab_v1/`: `codec.cpp:380-394,658-686,735-736,842-844` and `proto_descriptor.hpp:62-63,103-104` become compat-scoped v1 mappings (Wave-5 coordinates the codec split into `src/compat/eventgrab_v1/`; this wave creates the dir). `events.proto:10,41-42` stays (v1 wire is frozen/additive) but is served by the compat projection, not a core payload.
- Re-point storage: `jsonl_sink.cpp:273-288` `BrowserTab` overload removed; browser lines are written by the compat projection's v1 path.
- **Delete:** `tests/event/test_browser_bridge.cpp` / `test_browser_classifier.cpp` move to `tests/compat/eventgrab_v1/` and `tests/drivers/semantic/webextension/`; update the browser-coupled assertions in `tests/core/test_event.cpp:17,23,51,97-104`, `tests/event/test_event_descriptor.cpp:17,26,28,46,54,62`, `tests/transport/test_codec.cpp:67,192,200-203,277-279,366-367`, `tests/transport/test_service.cpp:639-640`, `tests/integration/test_exit_gate.cpp:44,426-447` to route through the compat projection.

**Interfaces:**
- Produces: core vocabulary with no `Browser` category/kind/payload; `eventgrab.v1` wire round-trip green **through** `src/compat/eventgrab_v1/`; `browser.tab_switched` re-derived from graph deltas.
- **Sequencing against the swap:** producer removed by T2 → `active_child.changed` deltas exist after T5 → compat projection + core-vocab removal land here (T7), last. Attempting the core-vocab removal before T5 would leave `browser.tab_switched` with no derivation path and break the v1 round-trip gate.

- [ ] **Step 1: Failing test** — `tests/compat/eventgrab_v1/test_browser_projection.cpp`: feed an `active_child.changed` delta with node app/title props; assert the projection emits a v1 `browser.tab_switched` with matching `tab_title`/`prev_tab_title`; assert `eventgrab.v1` codec round-trip green through compat.
- [ ] **Step 2:** run → fails. **Step 3:** implement projection + evidence adapter + relocate bridge. **Step 4:** remove `BrowserTab` from the core (atomic; fix all count-locked tables in the same commit) and re-point codec/proto/storage to compat. **Step 5:** update/move all browser tests.
- [ ] **Step 6:** full suite green (v1 round-trip through compat proven) → commit — `refactor(compat): demote browser to an eventgrab_v1 projection; core vocabulary browser-free (W4.7)`

### Task 8: JSONL storage envelope upgrade

**Files:**
- Modify: `src/storage/jsonl_sink.cpp:334-354` `serialize_line` — add `seq` (`event.sequence`), `origin` (`wire_name`/enum name of `event.origin`), `subject` (`{runtime,tree,epoch,node,revision}` when present), `cause` (`event.cause` OperationId), `before`/`after` (revisions) to the emitted object; assign **per-source sequence numbers** (monotonic per runtime/source) if not already stamped upstream.
- Test: `tests/storage/test_jsonl_sink.cpp` — assert a written line contains the new keys with correct values for an event carrying a full envelope; assert two runtimes produce distinct `subject.runtime` (depends on **T0**).

**Interfaces:**
- Consumed by: downstream JSONL readers; the audit-flagged data-loss fix.
- **RuntimeId coupling:** `subject.runtime` is only unambiguous once **T0** (session-level RuntimeId authority) lands. If T8 ships before T0, write `subject.runtime` for the primary runtime only and record the same deferral diagnostic noted at `session_impl.hpp:165-174`.

- [ ] Steps: failing test (envelope keys absent) → extend `serialize_line` + sequence numbering → suite green → commit — `feat(storage): JSONL envelope carries origin/subject/cause/revisions/sequence (W4.8)`

### Final: delete `src/event/` and close the wave

**Files:**
- Confirm `src/event/` is empty (every file relocated/deleted by T2–T7); remove the `grab_event` target from `CMakeLists.txt` (:132-145) and its links (`grab_driver_atspi:217`, `grab_service:262`, `grab_screen:154`, test targets); **delete the `src/event=5486` line** from `tests/scripts/legacy_budget.txt`; delete `include/grab/active_kind_probe.hpp` + its allowlist line (`public_header_allowlist.txt`).
- Test: full suite + `grab_invariant_checks` + `grab_legacy_ratchet` green.

- [ ] Steps: verify empty → remove target/links/budget/allowlist → suite green → commit — `refactor(event): delete the legacy src/event observation stack; budget line removed (W4.9)`

## Wave-4 exit checklist (orchestrator verifies before declaring the wave done)

1. `ctest` full suite green (socket/Xvfb flakes retried serially).
2. `src/event/` gone; `find src/event` empty; `src/event` + `src/platform` budget lines both absent from `legacy_budget.txt` (platform removed in W4.1).
3. `git grep -n 'PlatformFactory\|SourceRegistry\|MonitorSource\|BrowserTab\|EventCategory::Browser' -- src include` → empty (or only inside `src/compat/eventgrab_v1/`).
4. `eventgrab.v1` wire round-trip green **through** `src/compat/eventgrab_v1/`.
5. Demand-refcount ring-3 test proves XI2 masks toggle with subscriber count (T1/T2).
6. JSONL sample line carries `seq/origin/subject/cause/before/after` (T8).
7. `src/compat/x11_relocation_shim/` removed (its fenced consumers repointed) — or explicitly carried forward with a tracking note if `src/input` is still mid-purge.
8. `grab_invariant_checks` + `grab_legacy_ratchet` green.
9. Single-author history across the wave: `git log --format='%an <%ae> / %cn <%ce>' <wave-base>..HEAD | sort -u`.

## Known constraints carried from Wave 1 (do not re-discover)

- **TreeStore is single-scope; per-runtime stores landed.** `SessionCore` holds one `TreeStore` per `RuntimeBinding` (`session_impl.hpp:125-134,156`), so storage no longer collides. **But** X11 and AtSpi both mint `RuntimeId{1}` from their own generation counters (`session_impl.hpp:165-174`); a session-level RuntimeId authority is still pending — **Task 0 here**. The daemon's multi-runtime observation path (X11 + AtSpi + evdev pumping simultaneously, T2) needs it for `subject.runtime` correctness in the T8 envelope. Flagged, not assumed.
- **The runtime event sources only produce when pumped.** `wait_for_event` is pull-based (`spi/event_source.hpp:43-47`); there is no continuous pump today (deferred W1T8) — **Task 1 here** builds it. Until T1+T2 land, the canonical stack observes nothing live.
