# Purge Wave 1 — Composition & Public Verbs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> Parent plan: `docs/superpowers/plans/2026-07-16-legacy-purge.md` (Wave 1). Execution pipeline: Claude orchestrator drives Codex sol workers; Codex edits, orchestrator reviews/tests/commits (single-author rule).

**Goal:** Make the canonical stack the live stack: `Session` composes X11Runtime + TreeStore + TargetRegistry + EventBus, gains public `resolve/perform/capture/watch` verbs, the X11 runtime gets real event/topology sources with `EventOrigin` stamping, and the CLI/daemon bind through `grab::client` with table-driven dispatch.

**Architecture:** Wiring only — every engine used here (Transaction, WaitEngine, TreeStore, sel, routes, EventBus demand/replay) already exists and is unit-tested. New code is composition glue, six generic graph EventKinds, two X11 spi sources, and Transport verb extension. No legacy deletions except the duplicate CLI command table (W1.4).

**Tech Stack:** C++23, existing GTest + Xvfb fixture (`tests/CMakeLists.txt:150-166`, DISPLAY=:87), gRPC/protobuf wire at `proto/eventgrab/v1/`.

## Global Constraints

- Worktree `grab/.worktrees/integrate`, branch `feat/grab-port`. Repo git identity only; no AI attribution; verify single author per task commit.
- Build + `ctest --test-dir build --output-on-failure` green before every commit (re-run known X11 flakes serially before judging).
- Public headers: no `src/` includes, no vendor namespaces (`grab_invariant_checks` enforces). New public header (`watch.hpp`) must be added to `tests/scripts/public_header_allowlist.txt` **in the same commit** (Wave-0 ratchet enforces).
- Legacy dirs are frozen (LOC ratchet): all new code in `src/{kernel,spi,drivers,client,transport,service,cli}` and `include/grab/`. `src/core/session.cpp` is the one legacy-dir file this wave modifies — its budget line in `tests/scripts/legacy_budget.txt` may be **raised in the same commit as the Session composition change, with a comment**, or (preferred) new composition code goes in `src/kernel/lifecycle/session_impl.{hpp,cpp}` and `src/core/session.cpp` only delegates (small diff, budget untouched).
- Naming: CamelCase enum values, no `k` prefix, trailing `_` privates, `EnumTable` + `enum_table_has_count` static_asserts.
- Wire changes are additive only; `eventgrab.v1` round-trip tests must stay green.

## Verified seams (2026-07-16; re-verify lines before editing)

- `Session`/`Impl`: `include/grab/session.hpp:27-66`, impl `src/core/session.cpp` (owns Reactor only).
- `spi::Runtime`: `src/spi/runtime.hpp:28-95` (tree/topology/event_source, routes, action_route, input_seat).
- `TreeStore`: `src/kernel/graph/tree_store.hpp:62-103` — `apply(spi::UiUpdate) -> Result<AppliedDelta>` noexcept, `EventSink = std::function<void(const TreeEvent&)>`, `TreeEvent{kind,runtime,tree,epoch,revision,node,related,relation}`.
- `spi::TreeSource`: `src/spi/tree_source.hpp:62-83` — `snapshot(tree, ctx)`, `next_update(ctx) -> Result<std::optional<UiUpdate>>`.
- `Transaction`: `src/kernel/action/transaction.hpp:34-52` — `Transaction(spi::Runtime&, uint32_t tree, MappingRefreshHook).perform(Action, ActionOptions) -> TransactionOutcome{Receipt, optional<Error>}`.
- `Action/ActionOptions`: `include/grab/interaction.hpp:22-44` (`Click{ActionTarget}`, `TypeText{ActionTarget,text}`, `ActionTarget = variant<Locator,Match>`).
- `EventBus`: `include/grab/event_bus.hpp:121-183` — `publish`, `subscribe(SubscriptionScope, QueueOptions) -> Subscription` (has `id()`), `register_snapshot_provider(EventKind, SnapshotProvider)`, `set_demand_callback(DemandCallback = fn(EventKind, bool))`, `subscription_refcount`.
- Query: `src/kernel/query/evaluator.hpp` — `query::resolve(locator, cardinality, scope) -> Result<Match>`; `SnapshotTreeNav` at `src/kernel/query/snapshot_tree_nav.hpp:13-78`.
- `X11Runtime`: `src/drivers/desktop/x11/x11_runtime.hpp:27-89` (topology/event_source currently return nullptr at `x11_runtime.cpp:126-136`).
- `X11TreeSource` embeds its own `TargetRegistry` by value (`x11_tree_source.hpp:95`, const accessor `:50-51`); `AtspiTreeSource` takes an external `TargetRegistry&` — the shapes must converge (Task 2).
- `X11CaptureRoute`: `src/drivers/desktop/x11/x11_capture_route.hpp` — `open(display) -> Result<X11CaptureRoute>`, `capture_output(name) -> Result<Frame>`, exposes `CoordinateAuthority`/`SpaceGraph`.
- `EventKind` numbering: `include/grab/event.hpp:29-53` — 100s input, 200s window, 300s a11y, 400s app, 500 browser, 600 state. **700-block is free** for graph events.
- Envelope: `Event.origin/subject/cause/before_revision/after_revision` (`event.hpp:135-159`); wire parity table `src/transport/proto_descriptor.hpp` (parity static_asserts); codec `src/transport/codec.cpp`.
- CLI duplicate table: `src/cli/main.cpp:71-105`; canonical `commandDescriptors`: `include/grab/command_descriptor.hpp:72-145` (12 entries).
- Client: `src/client/{transport.hpp,client.hpp,loopback_transport.*,unix_socket_transport.*}` — Transport carries only push_event/subscribe/list_event_types.
- Exit-gate TODOs to eliminate: `tests/integration/test_exit_gate.cpp:299,319-322,378-381,399-404`.

Dependency order: T1 → T2 → T3 → {T4, T5, T6} → T7 → T8 → T9 (T4/T5/T6 parallelizable; T8 after T3; T9 last).

---

### Task 1: Generic graph EventKinds (700-block) + descriptors + wire parity

**Files:**
- Modify: `include/grab/event.hpp` (EventKind + payload), `include/grab/event_descriptor.hpp` (rows), `include/grab/payload_fields.hpp` (field names), `proto/eventgrab/v1/events.proto` (additive enum + payload message), `src/transport/proto_descriptor.hpp` (parity rows), `src/transport/codec.cpp` (encode/decode)
- Test: extend `tests/core/test_event_descriptor.cpp`, `tests/transport/test_codec.cpp`

**Interfaces:**
- Produces: `EventKind::{NodeAdded=700, NodeRemoved=701, NodeChanged=702, RelationAdded=703, RelationRemoved=704, ActiveChildChanged=705}`; payload `struct GraphChange{ std::uint64_t node; std::uint64_t related; std::uint32_t relation; std::uint64_t previous_active; }` added to the `Payload` variant; descriptor rows with wire names `"node.added"`, `"node.removed"`, `"node.changed"`, `"relation.added"`, `"relation.removed"`, `"active_child.changed"`, all `ReplayPolicy::None`, `CoalescingClass::NeverDrop` except `NodeChanged` = `Coalesce`.
- Subject/revision travel on the existing envelope (`Event.subject`, `before_revision`/`after_revision`) — no new envelope fields.

- [ ] **Step 1: Failing descriptor test** — in `tests/core/test_event_descriptor.cpp` add:

```cpp
TEST( EventDescriptor, GraphEventRowsExist )
{
    EXPECT_EQ( grab::wire_name_of( grab::EventKind::NodeAdded ), "node.added" );
    EXPECT_EQ( grab::wire_name_of( grab::EventKind::ActiveChildChanged ),
               "active_child.changed" );
    EXPECT_EQ( grab::coalescing_class_of( grab::EventKind::NodeChanged ),
               grab::CoalescingClass::Coalesce );
    EXPECT_EQ( grab::replay_policy_of( grab::EventKind::NodeAdded ),
               grab::ReplayPolicy::None );
}
```
(Adapt accessor names to the file's actual API — read `include/grab/event_descriptor.hpp` first; keep assertions semantically identical.)

- [ ] **Step 2: Run to verify failure** (compile error: no such enum values).
- [ ] **Step 3: Implement** — enum values, `GraphChange` payload struct + variant entry, descriptor rows (mind the descriptor-count static_assert), payload field names in `payload_fields.hpp`.
- [ ] **Step 4: Wire parity** — add the six kinds to `proto/eventgrab/v1/events.proto` (additive tags), a `GraphChange` proto message, parity rows in `proto_descriptor.hpp` (its static_asserts force completeness), encode/decode in `codec.cpp`. Round-trip test in `tests/transport/test_codec.cpp`:

```cpp
TEST( Codec, GraphChangeRoundTripsWithSubjectAndRevisions )
{
    grab::Event ev;
    ev.kind            = grab::EventKind::ActiveChildChanged;
    ev.category        = grab::EventCategory::Window;
    ev.before_revision = 41U;
    ev.after_revision  = 42U;
    ev.payload = grab::GraphChange{ .node = 7U, .related = 9U, .relation = 5U,
                                    .previous_active = 3U };
    const auto wire    = grab::transport::to_wire( ev );
    const auto decoded = grab::transport::from_wire( wire );
    ASSERT_TRUE( decoded.has_value() );
    EXPECT_EQ( decoded->after_revision, 42U );
    EXPECT_EQ( std::get<grab::GraphChange>( decoded->payload ).previous_active, 3U );
}
```
(Adapt to codec.cpp's actual to/from_wire signatures.)

- [ ] **Step 5: Full suite green → commit** — `feat(event): generic graph event vocabulary (node/relation/active-child) with wire parity (W1.1)`

### Task 2: X11TreeSource takes an external TargetRegistry

**Files:**
- Modify: `src/drivers/desktop/x11/x11_tree_source.{hpp,cpp}` (registry member `TargetRegistry` → `TargetRegistry&` injected via constructor), `src/drivers/desktop/x11/x11_runtime.{hpp,cpp}` (X11Runtime owns the `kernel::TargetRegistry` by value, passes it to the tree source; `target_registry()` accessor now returns the runtime-owned one)
- Test: `tests/drivers/x11/test_x11_tree_source.cpp` + `tests/drivers/atspi/test_atspi_alias.cpp` updated construction; new assertion that an AtspiTreeSource and X11TreeSource sharing ONE registry produce the exact-bridge fusion the alias tests already prove on a standalone registry (lift the fixture from `test_atspi_alias.cpp:94-108`, construct the registry once, hand it to both).

- [ ] **Step 1:** Write the shared-registry failing test (compile failure: X11TreeSource has no registry-injection constructor).
- [ ] **Step 2:** Refactor constructor + member; fix all construction sites (`grep -rn 'X11TreeSource(' src tests`).
- [ ] **Step 3:** Suite green → commit — `refactor(driver): X11 tree source shares an injected TargetRegistry (W1.1)`

### Task 3: Session composition root

**Files:**
- Create: `src/kernel/lifecycle/session_impl.hpp`, `src/kernel/lifecycle/session_impl.cpp` (class `grab::kernel::lifecycle::SessionCore`)
- Modify: `src/core/session.cpp` (Impl gains `std::unique_ptr<kernel::lifecycle::SessionCore> core_;` constructed in `open()` when DISPLAY resolves; delegation only — keep the diff in this legacy file minimal), `CMakeLists.txt` (new sources into `grab_core`; link `grab_driver_x11`)
- Test: `tests/kernel/lifecycle/test_session_core.cpp` (FakeRuntime-based, display-free) + extend `tests/integration/test_exit_gate.cpp` two-sessions block

**Interfaces:**
- Produces:
```cpp
namespace grab::kernel::lifecycle
{
    class SessionCore
    {
        public:
            // Owns bus/store/registry; attaches runtimes; pumps tree updates.
            [[nodiscard]] static Result<std::unique_ptr<SessionCore>>
            open( const SessionOptions& options );          // starts X11Runtime; AT-SPI best-effort (Task 7)

            [[nodiscard]] Result<void> attach( spi::Runtime& runtime,
                                               const OperationContext& context );  // snapshot -> store, register pump
            [[nodiscard]] EventBus&               bus() noexcept;
            [[nodiscard]] kernel::TreeStore&      store() noexcept;
            [[nodiscard]] kernel::TargetRegistry& registry() noexcept;
            [[nodiscard]] spi::Runtime&           primary_runtime() noexcept;      // X11
            [[nodiscard]] Result<void>            pump_once( const OperationContext& );  // drain next_update() from every attached source into the store
    };
}
```
- TreeStore EventSink → bus translation: `TreeEvent{NodeAdded,...}` → `Event{kind=EventKind::NodeAdded..., category=Window, subject={runtime,tree,epoch,node,revision}, before_revision=<previous>, after_revision=<revision>, payload=GraphChange{...}}`. `ActiveChildChanged` is emitted when a `RelationAdded/Removed` pair carries `relation == relation::active_child` (map add→current, removed→previous_active).
- Consumed by Tasks 4/5/6 (verbs call `core_->...`).

- [ ] **Step 1: Failing FakeRuntime test** — `tests/kernel/lifecycle/test_session_core.cpp`:

```cpp
TEST( SessionCore, AttachedFakeRuntimeSnapshotReachesStoreAndBus )
{
    grab::testing::FakeRuntime fake;                     // tests/fake/fake_runtime.hpp
    fake.inject_snapshot( /* one-node snapshot, reuse the fixture builder
                              from tests/kernel/test_tree_store.cpp */ );
    auto core = grab::kernel::lifecycle::SessionCore::open_for_test();  // no display: bus/store only
    auto watch = core->bus().subscribe(
        grab::SubscriptionScope{ .kinds = { grab::EventKind::NodeAdded }, .filter = {} } );
    grab::OperationContext ctx;
    ASSERT_TRUE( core->attach( fake, ctx ).has_value() );
    const auto ev = watch.try_pop();
    ASSERT_TRUE( ev.has_value() );
    EXPECT_EQ( ev->kind, grab::EventKind::NodeAdded );
    EXPECT_EQ( ev->after_revision, core->store().revision() );
}
```
Add `open_for_test()` (no runtime construction) alongside `open()`. Reuse the snapshot-builder helpers already in `tests/kernel/test_tree_store.cpp` (move them to a shared `tests/kernel/tree_fixtures.hpp` if needed).

- [ ] **Step 2:** Run → fails (no such header). **Step 3:** Implement SessionCore (attach = `tree_source->snapshot` → `store.apply` → sink publishes; pump_once drains `next_update`). **Step 4:** green.
- [ ] **Step 5: Wire into Session** — `Session::open` constructs SessionCore when a display is available; `Session` keeps behavior otherwise unchanged. Two-sessions exit-gate block gains an isolation assertion: publish onto session A's bus, assert session B's subscription sees nothing.
- [ ] **Step 6:** Full suite green (incl. Xvfb tests) → commit — `feat(session): composition root — X11 runtime + TreeStore + TargetRegistry + bus wiring (W1.1)`

### Task 4: Public resolve + watch (+ `include/grab/watch.hpp`)

**Files:**
- Create: `include/grab/watch.hpp` (re-export `Subscription`, `SubscriptionScope`, `QueueOptions`, `SubscriptionEvent`, `QueueGapMarker` — thin include of `event_bus.hpp` for now; the internalization of `event_bus.hpp` happens in Wave 6)
- Modify: `include/grab/session.hpp` + `src/core/session.cpp` (delegating), `src/kernel/lifecycle/session_impl.{hpp,cpp}`, `tests/scripts/public_header_allowlist.txt` (+`watch.hpp`), `include/grab/locator.hpp` (convenience `resolve` free-function declaration lives in `query.hpp` — keep Locator pure)
- Test: `tests/kernel/lifecycle/test_session_verbs.cpp`

**Interfaces:**
- Produces on `grab::Session`:
```cpp
[[nodiscard]] Result<Match>        resolve( const Locator& locator,
                                            Cardinality cardinality = Cardinality::ExactlyOne );
[[nodiscard]] Result<Subscription> watch( SubscriptionScope scope, QueueOptions options = {} );
```
- `resolve` = fresh `core_->store().snapshot()` → `SnapshotTreeNav` → `query::resolve` (exact seam used by `src/kernel/action/transaction.cpp:313-318`).

- [ ] **Step 1: Failing tests** (FakeRuntime; one-node snapshot with role `role::Window`):

```cpp
TEST( SessionVerbs, ResolveFindsNodeAndWatchYieldsSubscriptionId )
{
    /* build SessionCore with fake runtime as in Task 3 */
    const auto match = core->resolve( grab::locator().role( grab::role::Window ),
                                      grab::Cardinality::ExactlyOne );
    ASSERT_TRUE( match.has_value() );
    auto sub = core->watch( { .kinds = { grab::EventKind::NodeChanged }, .filter = {} } );
    ASSERT_TRUE( sub.has_value() );
    EXPECT_NE( sub->id(), grab::SubscriptionId{} );
}
```
(SessionCore-level first; then the same through `grab::Session` in the Xvfb exit-gate test in Task 9.)

- [ ] **Step 2-4:** fail → implement → green. **Step 5:** allowlist += `watch.hpp` same commit. Commit — `feat(session): public resolve + watch verbs with server-issued SubscriptionId (W1.2)`

### Task 5: Public perform → Receipt

**Files:**
- Modify: `include/grab/session.hpp`, `src/core/session.cpp`, `src/kernel/lifecycle/session_impl.{hpp,cpp}`
- Test: `tests/kernel/lifecycle/test_session_verbs.cpp` (FakeRuntime route scripting per `tests/kernel/action/test_transaction.cpp:85-130` pattern)

**Interfaces:**
- Produces: `Session::perform( const Action& action, const ActionOptions& options = {} ) -> Result<Receipt>` — constructs `kernel::action::Transaction{ core_->primary_runtime(), tree, mapping_refresh }` where `mapping_refresh` returns the capture authority's transforms once Task 6 lands (empty hook until then); on `TransactionOutcome.error` returns the error (receipt still recorded in the error's diagnostics is NOT required — return `outcome.receipt` on success only).
- **Caller-supplied cancellation:** add `std::stop_token stop{}` to `ActionOptions` (audit finding: `Transaction::perform` builds its own context; thread the token through so cancellation is reachable — extend `Transaction::perform` to accept it via `ActionOptions`).

- [ ] **Step 1: Failing test:**

```cpp
TEST( SessionVerbs, PerformClickReturnsReceiptWithCommitStatus )
{
    /* FakeRuntime with scripted pointer route (see test_transaction.cpp:85) */
    const auto receipt = core->perform(
        grab::Click{ grab::locator().role( grab::role::Window ) }, {} );
    ASSERT_TRUE( receipt.has_value() );
    EXPECT_EQ( receipt->commit_status, grab::CommitStatus::Committed );
    EXPECT_FALSE( receipt->routes.empty() );
}

TEST( SessionVerbs, PerformHonorsCallerStopToken )
{
    std::stop_source stop;
    stop.request_stop();
    const auto receipt = core->perform(
        grab::Click{ grab::locator().role( grab::role::Window ) },
        { .stop = stop.get_token() } );
    ASSERT_FALSE( receipt.has_value() );
    EXPECT_EQ( receipt.error().code, grab::ErrorCode::Cancelled );
}
```
(Adapt Receipt field names to `include/grab/trace.hpp:73-91`.)

- [ ] **Step 2-4:** fail → implement (incl. threading `ActionOptions.stop` into the transaction's OperationContext at `transaction.cpp:659-678`) → green. Commit — `feat(session): public perform returning Receipt; caller stop-token reaches the transaction (W1.2)`

### Task 6: Public capture → Frame (runtime-owned route)

**Files:**
- Modify: `src/drivers/desktop/x11/x11_runtime.{hpp,cpp}` (owns `std::optional<X11CaptureRoute>` opened at `start()` on the same display string; accessor `capture_route()`), `include/grab/session.hpp`, `src/kernel/lifecycle/session_impl.{hpp,cpp}`
- Test: `tests/drivers/x11/test_x11_capture_route.cpp` (un-disable by routing through the Xvfb fixture like `test_x11_runtime.cpp` does), `tests/kernel/lifecycle/test_session_verbs.cpp`

**Interfaces:**
- Produces: `Session::capture( const CaptureTarget& target, CaptureOptions options = {} ) -> Result<Frame>` with `using CaptureTarget = std::variant<std::string /*output name*/, Match /*window-grade node*/>;` and `struct CaptureOptions{ std::chrono::nanoseconds deadline{ std::chrono::seconds{ 2 } }; };` (in `include/grab/capture.hpp`). Window-target capture resolves the Match's window alias via `core_->registry()` and captures its output region (region-capture variant exists at `src/screen/x11_capture.cpp:1227-1297`).
- Note: full single-connection unification (capturer sharing the runtime's XcbConnection) is **Wave 3** scope; here the runtime owns/opens the route so route lifetime and display authority are runtime-scoped (parent-plan W1 gate satisfied organizationally; record in commit message).

- [ ] **Step 1:** Failing Xvfb test: `Session::open` → `capture("<output name from list>")` → Frame has nonzero `FrameId`, `space != CoordinateSpaceId{}`, `generation != CaptureGeneration{}`.
- [ ] **Step 2-4:** fail → implement → green (this also un-disables `DISABLED_CaptureOutputProducesFrame` — rename to drop the prefix and fixture-gate it).
- [ ] **Step 5:** Hook Task 5's `mapping_refresh` to the route's `SpaceGraph` transforms. Commit — `feat(session): public capture returning Frame via runtime-owned capture route (W1.2)`

### Task 7: AT-SPI runtime attached (best-effort)

**Files:**
- Modify: `src/kernel/lifecycle/session_impl.cpp` (`open()` constructs `AtspiRuntime` with the shared `TargetRegistry`; `start` failure → recorded diagnostic, not session failure), `CMakeLists.txt` (link `grab_driver_atspi` into `grab_core`)
- Test: `tests/kernel/lifecycle/test_session_core.cpp` — assert a session opens cleanly when AT-SPI is unavailable and `doctor`-visible reason is stored (accessor `SessionCore::runtime_diagnostics()` returning `std::vector<DiagnosticEntry>`).

- [ ] Steps: failing test (no-bus environment must still open; diagnostics non-empty mentioning atspi) → implement → green → commit — `feat(session): best-effort AT-SPI runtime attach with recorded diagnostics (W1.1)`

> **Execution finding (Task 7, commit `1939981`) — TreeStore is single-scope.** `TreeStore::apply` retires any prior runtime scope when an update carries a different `RuntimeId` (`tree_store.cpp:1358-1370`) and permanently rejects retired ids; worse, `X11TreeSource` and `AtspiTreeSource` both mint `RuntimeId{1}`, so two live sources would collide on scope identity. AT-SPI attach is therefore **deferred** (recorded diagnostic) until this is resolved. Resolution direction per the canonical plan's §1 storage rule ("each runtime publishes its own versioned snapshots; composite views are query-time projections"): **one TreeStore per attached runtime** inside SessionCore plus a session-level RuntimeId allocation authority — NOT a multi-scope store. Scheduled as **Task 7b** (own bite-size addendum) after Task 9; the Wave-1 exit gate's semantic arm remains satisfied by the deferral diagnostic until 7b lands.

### Task 8: X11 event + topology sources, EventOrigin stamping, demand wiring

**Files:**
- Create: `src/drivers/desktop/x11/x11_event_source.{hpp,cpp}` (implements `spi::EventSource` from `src/spi/event_source.hpp:33-47` over XI2 raw events on the runtime's connection: `wait_for_event` blocks on the XCB fd with deadline; `enable(spec)/disable(spec)` toggles XI2 event masks), `src/drivers/desktop/x11/x11_topology_source.{hpp,cpp}` (implements `spi::TopologySource` over RandR: emits topology change records; feeds `CoordinateAuthority::refresh`)
- Modify: `x11_runtime.{hpp,cpp}` (return the sources), `src/kernel/lifecycle/session_impl.cpp` (`bus().set_demand_callback` → map EventKind→enable/disable on the runtime's event source — the audit's missing demand propagation), X11InputSeat (`x11_routes.cpp:534-644`): record an injection ledger `{device, detail, timestamp-window}` on every XTest send; X11EventSource consults it — XI2 events whose source device is an XTEST slave are `InjectedSelf` when ledger-matched, `InjectedOther` otherwise; all other devices `Physical`.
- Test: `tests/drivers/x11/test_x11_event_source.cpp` (Xvfb): (a) enable(KeyDown) → inject via seat → event arrives with `origin == InjectedSelf`; (b) `subscription_refcount` 1→0 drives mask removal (assert via a second injection producing no event); (c) demand callback fires on bus subscribe/unsubscribe.

- [ ] Steps: failing tests → implement source + ledger + demand plumbing → green → commit — `feat(driver): X11 event/topology sources with EventOrigin stamping and demand-driven XI2 masks (W1.3)`

### Task 9: CLI/daemon rebind + exit-gate rewrite (closes Wave 1)

**Files:**
- Modify: `src/client/transport.hpp` (+`resolve/perform/capture` verbs mirroring Session signatures, wire-encodable args), `src/client/loopback_transport.{hpp,cpp}` (implement over an owned `Session`), `proto/eventgrab/v1/service.proto` (+`PerformAction`, `CaptureFrame` RPCs — additive; request/response messages serialize Locator canonical string, ActionOptions fields, Receipt summary, PNG-encoded frame bytes + FrameId/space/generation), `src/transport/service.{hpp,cpp}` (handlers dispatch through `commandDescriptors` — look up by name, honor `mutability/consent_gated`; admission-wrap via the existing `dispatch`), `src/client/unix_socket_transport.{cpp,hpp}`, `src/cli/main.cpp` (verb dispatch table generated from `commandDescriptors` — **delete the private Command enum + commandNames table at `src/cli/main.cpp:71-105`**; each verb builds client calls), `src/cli/input_command.cpp`
- Test: `tests/client/test_client.cpp` (+perform/capture through both transports against FakeRuntime-backed Session/loopback and TransportServer/socket), `tests/transport/test_service.cpp` (+descriptor-driven dispatch: unknown command name → typed error; consent_gated command rejected without grant), rewrite `tests/integration/test_exit_gate.cpp` to use ONLY `grab::Session` + `grab::client` public verbs — delete the four `TODO(phase1)` blocks (`:299,319,378,399`)

**Interfaces:**
- Produces: `Transport::perform(const Action&, const ActionOptions&) -> Result<Receipt>`, `Transport::capture(const CaptureTarget&, const CaptureOptions&) -> Result<Frame>`, `Transport::resolve(const Locator&, Cardinality) -> Result<Match>`; CLI verbs `click/type/drag/key/capture/watch` routed through a `Client` (loopback by default, `--endpoint` for daemon).
- Gate evidence (parent-plan Wave-1 gate): exit-gate green through public verbs; `grep -rn 'TODO(phase1)' tests/` → zero; `grep -c 'grab::client' src/cli/*.cpp` ≥ 1 per rebound verb file; duplicate CLI table gone.

- [ ] Steps per verb (client-loopback first, wire second, CLI rebind third, exit-gate rewrite last), full suite + Xvfb serially → commit sequence:
  1. `feat(client): transport verbs resolve/perform/capture over loopback (W1.4)`
  2. `feat(transport): PerformAction/CaptureFrame RPCs with descriptor-driven dispatch (W1.4)`
  3. `feat(cli): verbs bound through grab::client; single command table (W1.4)` ← deletes `main.cpp:71-105`
  4. `test(integration): exit gate through public Session/client verbs only (W1 gate)`

## Wave-1 exit checklist (orchestrator verifies before declaring the wave done)

1. `ctest` full suite green (Xvfb flakes retried serially).
2. `grep -rn 'TODO(phase1)' tests/ src/ include/` → empty.
3. `grep -rln 'X11Runtime' src/ | grep -v drivers` → non-empty (live composition exists).
4. Two-sessions isolation assertion passing.
5. `grab_invariant_checks` + `grab_legacy_ratchet` green (allowlist updated only by Task 4; budgets untouched or raised-with-comment only by Task 3's session.cpp delegation).
6. Single-author history across the wave: `git log --format='%an <%ae> / %cn <%ce>' <wave-base>..HEAD | sort -u`.
