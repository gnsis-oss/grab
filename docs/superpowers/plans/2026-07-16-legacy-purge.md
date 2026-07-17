# Legacy-Code Purge Plan (wire-then-delete)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Wave 0 is fully specified below; Waves 1–6 are dependency-ordered decompositions that **each get their own bite-size plan before execution** (repo convention, same as the canonical plan's Phase 1).

**Goal:** Remove the pre-plan legacy stack (~14k LOC across `src/{event,input,screen,platform,core}` + legacy public headers) by first wiring its canonical-architecture replacement into the live path, then deleting — never the reverse.

**Architecture:** The 2026-07-16 audit (`PLAN-AUDIT-2026-07-16-canonical-vs-integrate.md`, workspace root) found the new kernel/runtime/client stack fully built but test-only, while the legacy stack still runs the daemon, CLI, and public API. Every purge therefore follows one law: **wire → cut over → delete**, per subsystem, behind CI ratchets that make legacy code shrink-only from day one.

**Tech Stack:** C++23, CMake ≥ 3.28, GTest, existing `feat/grab-port` tree at `8f4e35e`. No new external dependencies.

## Global Constraints

- Branch/worktree: all work on `feat/grab-port` in `grab/.worktrees/integrate`. Never touch the primary checkout.
- Git identity: repo-configured user only. **No `Co-Authored-By`, no AI attribution, no `--author` overrides — ever.** Verify per wave: `git log --format='%an <%ae> / %cn <%ce>' <wave-base>..HEAD | sort -u` → exactly one identity.
- Commit style: `type(scope): summary`. Build + `ctest --test-dir build --output-on-failure` green before every commit (X11 flakes: re-run serially before judging).
- **ALL means ALL (repo CLAUDE.md §9):** no CLI verb, public API behavior, or wire message may be silently dropped by a purge. Any intentional break is recorded in the wave's plan and in the commit message.
- Naming: CamelCase enum values, no `k` prefix, trailing `_` on private members, `EnumTable` + count static_asserts.
- Spec of record: `docs/superpowers/specs/2026-07-13-canonical-architecture.md`; on divergence fix spec or plan in the same commit.

## The Purge Law (gates for every deletion)

A legacy file/dir may be deleted only when ALL of:

- **G1 — wired:** its replacement is constructed on the live path (daemon composition, CLI dispatch, or public API), not only in tests.
- **G2 — parity:** every behavior the legacy code delivered is preserved through the replacement or explicitly waived in the wave plan (CLI verbs enumerated one by one).
- **G3 — tests migrated:** the legacy unit's tests are re-pointed at the replacement or deleted with it; full suite green.
- **G4 — wire compat:** `eventgrab.v1` round-trip stays green (via `src/compat/eventgrab_v1/` once it exists).
- **G5 — ratchet:** the corresponding `legacy_budget.txt` line is lowered (or removed) in the same commit — budgets are shrink-only.
- **G6 — authorship:** single-identity check passes.

## Inventory disposition (measured 2026-07-16)

**REPLACE-THEN-DELETE (the purge targets, ~14k LOC):**

| Dir | LOC | Replacement | Wave |
|---|---|---|---|
| `src/event/` | 5,486 | `spi::Runtime` sources under `src/drivers/{desktop/x11, semantic/atspi, semantic/webextension, device/evdev}` + TreeStore-derived events + `src/compat/eventgrab_v1/` | 4 |
| `src/screen/` | 4,876 | capture routes + `Frame` + kernel capture engine (`TileDiffer`/`InjectGate`) + facades; `virtual_display.cpp` relocates to the session/workspace domain | 3 |
| `src/input/` | 2,395 | kernel `Transaction` + X11 routes + seat correctness kit + thin `Input` facade | 2 |
| `src/platform/` | 1,243 | **relocate** (not rewrite) under `src/drivers/desktop/x11/` — one X11 authority | 4 |
| `src/core/` | 4,836 | **dissolve** into named §12 concerns (see Wave 6 table) — the plan bans the `core/` dumping ground | 6 |

**Legacy public headers to demote/delete** (Wave 6, after facades land): `pid.hpp` (consumers die in Wave 4), `event_bus.hpp` (internalize once `watch.hpp` is the public surface), `active_kind_probe.hpp` (consumer `source_registry` dies in Wave 4; `transport/service.cpp` + `event_descriptor.hpp` re-pointed), `payload_fields.hpp` (internalize), `window_match.hpp`/`pointer_button.hpp`/`drag.hpp`/`keymap.hpp`/`screen.hpp`/`input.hpp` (reshape or fold per facade outcome; §12 target list is the arbiter).

**KEEP (not purge targets):** `src/codec/` (PNG codec — a shipped feature), `src/image/` (comparison), `src/notify/`, `src/storage/` (schema upgrade in Wave 4 ≠ purge), `src/session/` (Workspace domain; only its `Pid` bookkeeping migrates), `src/cli|transport|service` (relocate to `src/frontends/` in Wave 5, not deleted), everything under `src/{kernel,spi,drivers,client,vendor}`.

Dependency DAG:

```
W0 ─► W1 ─► W2 ─┐
        ├─► W3 ─┼─► W6
        ├─► W4 ─┤
        └─► W5 ─┘        (W2/W3/W4/W5 parallelize after W1; W6 strictly last)
```

---

## Wave 0 — Ratchets (executable now; no behavior change)

### Task 1: Legacy LOC ratchet as a ctest

**Files:**
- Create: `tests/scripts/check_legacy_ratchet.sh`, `tests/scripts/legacy_budget.txt`, `tests/scripts/public_header_allowlist.txt`
- Modify: `tests/CMakeLists.txt` (register `grab_legacy_ratchet` next to `grab_invariant_checks` at `tests/CMakeLists.txt:1`)

**Interfaces:**
- Produces: ctest `grab_legacy_ratchet` failing when (a) any budgeted legacy dir grows past its LOC budget, (b) a budget line names a dir that no longer exists (stale line), or (c) `include/grab/` contains a header not in the allowlist. Both data files are shrink-only.

- [ ] **Step 1: Write the budget file** — `tests/scripts/legacy_budget.txt`:

```
# format: <repo-relative dir>=<max total LOC across *.cpp/*.hpp>
# shrink-only: purge waves lower these numbers; deleting a dir deletes its line.
src/core=4836
src/event=5486
src/input=2395
src/platform=1243
src/screen=4876
```

- [ ] **Step 2: Write the allowlist** — generate, then commit the output:

Run: `find include/grab -name '*.hpp' | sed 's|^include/grab/||' | sort > tests/scripts/public_header_allowlist.txt`
Expected contents (36 lines): `active_kind_probe.hpp capability.hpp capture.hpp command_descriptor.hpp context.hpp drag.hpp enum_table.hpp event.hpp event_bus.hpp event_descriptor.hpp geometry.hpp geometry/curve.hpp geometry/point.hpp geometry/rectangle.hpp geometry/size.hpp ids.hpp image.hpp input.hpp interaction.hpp keymap.hpp locator.hpp origin.hpp payload_fields.hpp pid.hpp pointer_button.hpp presentation.hpp process_ref.hpp query.hpp relation.hpp result.hpp role.hpp screen.hpp session.hpp space.hpp trace.hpp ui.hpp version.hpp window_match.hpp workspace.hpp` (one per line; keep whatever `find` actually emits — the observed list is authoritative). Prepend the header comment:

```
# format: one repo-relative header path under include/grab/ per line.
# shrink-only toward the spec §12 target list; new public headers require a plan task.
```

- [ ] **Step 3: Write the script** — `tests/scripts/check_legacy_ratchet.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
fail=0
note() { echo "legacy ratchet violation: $1"; fail=1; }

# 1. Budgeted legacy dirs may only shrink.
while IFS='=' read -r dir budget; do
    case "$dir" in ''|\#*) continue ;; esac
    if [[ ! -d "$dir" ]]; then
        note "$dir is gone — delete its stale budget line"
        continue
    fi
    loc=$(find "$dir" \( -name '*.cpp' -o -name '*.hpp' \) -print0 | xargs -0 --no-run-if-empty cat | wc -l)
    if (( loc > budget )); then
        note "$dir grew to $loc LOC (budget $budget) — legacy is frozen; new code goes in src/{kernel,spi,drivers,frontends,compat}"
    fi
done < tests/scripts/legacy_budget.txt

# 2. No unlisted public headers.
while IFS= read -r f; do
    rel="${f#include/grab/}"
    grep -qxF "$rel" tests/scripts/public_header_allowlist.txt \
        || note "unlisted public header: include/grab/$rel"
done < <(find include/grab -name '*.hpp' | sort)

exit "$fail"
```

- [ ] **Step 4: Register the ctest** — in `tests/CMakeLists.txt`, mirror the `grab_invariant_checks` registration (lines 1-2):

```cmake
add_test(NAME grab_legacy_ratchet
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_legacy_ratchet.sh)
```

`chmod +x tests/scripts/check_legacy_ratchet.sh`.

- [ ] **Step 5: Verify it fails on growth, passes clean** — Run `ctest --test-dir build -R grab_legacy_ratchet --output-on-failure` → PASS. Then `echo '// x' >> src/event/source.hpp && ctest -R grab_legacy_ratchet` → FAIL with "src/event grew"; `git checkout src/event/source.hpp`; re-run → PASS.

- [ ] **Step 6: Commit**

```bash
git add tests/scripts/check_legacy_ratchet.sh tests/scripts/legacy_budget.txt \
        tests/scripts/public_header_allowlist.txt tests/CMakeLists.txt
git commit -m "test(ci): legacy LOC ratchet + public-header allowlist (purge Wave 0)"
```

### Task 2: Track this plan

- [ ] **Step 1:** `docs/` is gitignored (`.gitignore:14`) — force-add:

```bash
git add -f docs/superpowers/plans/2026-07-16-legacy-purge.md
git commit -m "docs(plan): legacy purge plan (wire-then-delete waves)"
```

---

## Wave 1 — Composition & public verbs (the enabler; wiring only)

**Objective:** make the new stack the live stack. This wave deletes almost nothing but unlocks every later deletion. It is the audit report's §11 items 1–3, restated as the purge precondition.

| # | Task | Produces (gate evidence) |
|---|---|---|
| W1.1 | Session composition root: `Session::Impl` builds X11Runtime + AtspiRuntime + `TreeStore` + `TargetRegistry`; runtime tree sources pump the store; store `TreeEvent`s publish onto the EventBus as generic graph EventKinds (`node.added/removed/changed`, `relation.changed`, `active_child.changed`) | daemon + Session construct runtimes (grep gate: `X11Runtime` consumers outside tests ≥ 2) |
| W1.2 | Public verbs on `Session`: `perform(Click/TypeText/Drag, ActionOptions) -> Result<Receipt>` over kernel `Transaction`; `capture(target, CaptureOptions) -> Result<Frame>` over capture routes; `watch(...) -> Subscription` (server-issued `SubscriptionId`); new `include/grab/watch.hpp`; `Locator::resolve()/exactly_one()` conveniences | the four `TODO(phase1)` comments in `tests/integration/test_exit_gate.cpp:299,319,378,399` deleted; exit gate rewritten to public verbs only |
| W1.3 | X11Runtime completion: real `event_source()` (XI2 wrap) + `topology_source()` (RandR); capture route owned by the runtime (one authority, no second display connection); `EventOrigin` stamped at the XI2/evdev edge + synthesis self-loopback (`InjectedSelf`) | no `nullptr` sources; origin ≠ `Unknown` on observed input in a ring-3 test |
| W1.4 | Wire + CLI rebind: capture/input RPCs added to the proto, dispatch driven by `commandDescriptors` (table gains its consumers); CLI verb dispatch generated from the same table; CLI verbs call `grab::client` | **delete** the duplicate CLI command table (`src/cli/main.cpp:71-105`); drift impossible by construction |

Gate to advance: exit gate green through public verbs; daemon runs runtimes; ratchet unchanged (legacy untouched, frozen).

## Wave 2 — Input purge (`src/input/` 2,395 → ~150 facade LOC)

Prereq: W1. Port before delete:

- `gestures.cpp:338` (linear/curve drag, menu click) → gesture recipes executed through the X11 pointer route inside `Transaction` commits, preserving `DragOptions` semantics + the drag-curve CLI verb; burns its allowlisted sleep (`sleep_allowlist.txt` shrinks).
- `locator.cpp:943` (window location + activation polling) → `grab::sel` locator + a new `window_mapped` WaitEngine predicate (the audit found it missing); burns its allowlisted sleep.
- `input.cpp:664` → thin facade: every `Input` method delegates to `Session::perform`; `seat.cpp:309` (legacy stateless XTest seat) deleted — X11InputSeat + `ModifierGuard` + `ScratchKeycodePool` + `acquire_lane` become the only synthesis path (gaining their first live consumers).

**Delete:** `src/input/{seat,gestures,locator}.{cpp,hpp}`; rewrite `src/input/input.cpp` as facade. Budget: `src/input=2395` → `150`. Tests `tests/input/*` re-pointed at routes/transaction/facade.

> **Wave-1 execution correction (2026-07-16, verified in code) — this wave is larger than the sketch above.** Three facts the parent sketch missed:
> 1. **`src/input/seat.{cpp,hpp}` (309 LOC) is NOT deletable — it is the live XTest primitive.** Wave-1's `X11InputSeat` holds a `grab::input::Seat` **by value** (`src/drivers/desktop/x11/x11_routes.hpp:67`; constructed at `x11_routes.cpp:533`, `x11_runtime.cpp:68`). The correctness kit *wraps* the legacy seat, it does not replace it. Correct action: **relocate** `seat.{cpp,hpp}` under `src/drivers/desktop/x11/` (verbatim move, like `src/platform` in Wave 4) — or inline its XTest bodies into `X11InputSeat` — never delete. The `src/input` budget target is therefore **~150 + wherever seat lands**, and seat's LOC moves to the (unbudgeted) driver tree.
> 2. **The `Action` vocabulary has only `Click`/`TypeText`** (`include/grab/interaction.hpp:35-46`); there is no `Drag` or `PressKey` verb and no `spi::ActionVerb` for them (`transaction.cpp:454-465`). So `Input::drag/drag_curve_in_window/press_key` and the CLI `drag/drag-curve/key` verbs **cannot** become `Session::perform` facades until the vocabulary is extended. **New Wave-2 prerequisite task (2a): add `Drag`/`PressKey` to `Action` + `spi::ActionVerb` + the X11 pointer/keyboard routes + transaction dispatch**, TDD, before any facade collapse.
> 3. **`Input::locate/activate` have no Session equivalent** (no locate verb; `resolve` returns a `Match`, not a window-activation). `locator.cpp`'s activation path needs the `window_mapped` WaitEngine predicate *and* a `Session`-level activate/locate convenience before `Input` can shed it.
>
> Revised Wave-2 task order: **2a** vocabulary+routes (Drag/PressKey) → **2b** `window_mapped` predicate + sel-based location → **2c** relocate `seat` under the X11 driver (budget line edit) → **2d** port `gestures` to route-driven recipes, delete `gestures.{cpp,hpp}` → **2e** replace `locator` usage, delete `locator.{cpp,hpp}` → **2f** collapse `input.cpp` to a Session-verb facade. Budget ends at `src/input = ~150` with `seat` relocated, not deleted.
>
> **Wave-2 execution status (2026-07-16, branch `purge/wave2-input` @ `4641e99`, 2a-2d DONE, single-author, green):** 2a-2d landed (Drag/PressKey verbs + routes; `window_mapped` predicate; seat relocated to `src/drivers/desktop/x11/x11_xtest_seat.*`; gestures deleted via `x11_drag_recipe.hpp`; `src/input` 2395→1665). **2e/2f blocked on a THIRD missing primitive: window activation.** `WindowLocator` does two jobs — *locate* (replaceable by `Session::resolve(sel::window)` + node `bounds`, already stamped by `x11_tree_source.cpp:160-168`) and *activate* (EWMH `_NET_ACTIVE_WINDOW` client-message + raise + poll, `locator.cpp:874-928`, the allowlisted sleep at `:739`). Activation has **no verb, route, or primitive** anywhere else (`grep _NET_ACTIVE_WINDOW\|set_input_focus src/drivers src/kernel` → empty) — same live-primitive-with-no-replacement class as the seat. **Revised tail: insert 2a-shaped Task 2e0 — add an `Activate`/`RaiseWindow` verb to `Action`+`spi::ActionVerb` + an X11 activation route that lifts the EWMH body out of `locator.cpp` (relocation) and waits on `window_mapped`, TDD.** Then land **2e+2f as ONE commit** (they're inseparable — both need a `Session` inside `input.cpp`: location via sel+bounds, activation via the new verb, `input.cpp` collapsed to facade, CLI rebound, `locator.{cpp,hpp}` deleted). Also **build-config trap:** the default `build/` must be configured with `cmake --preset default` (clang++23 + Ninja); a GCC+Make config fails at base on `-Werror=shadow` in the frozen `src/platform/x11/xi_seat.cpp`.
>
> **Wave-2 tail RULING (2026-07-16, `purge/wave2-input` @ `91df525` — 2e0 `Activate` verb DONE).** 2e+2f hit a public-API blocker (verified): (1) raw-coordinate verbs `click --at` / absolute `drag` / `type`-to-focused have no target-based `perform` representation (no absolute-coordinate/pointerless verb, no `sel::active()/focus()`, nodes never carry `Focused`); (2) no public path from a `Match` to a node's `bounds` (`resolve` returns a ref-only `Match`; `property::bounds` is snapshot-internal). **Ruling (aligns with canonical-plan migration rows 3+4 and §5):** `grab::Input` becomes the **raw-seat-only** facade — `move/click/click_at/drag/type_text/press_key`, single X11 connection via the relocated XTest seat — and the window-semantic methods (`locate/activate/click_in_window/drag_curve_in_window`) + `LocatedWindow` **leave public `input.hpp`**, reimplemented in the CLI over Session verbs. Prerequisite **Task 2e1**: add a public geometry read — `Session::describe(const Match&) -> Result<NodeInfo{bounds: SpaceRect, states, role, provenance}>` (the agent surface needs this anyway). Then **2e2f**: CLI `locate`=`resolve(sel::window)`+`describe`, `activate`=`Activate` verb, `click_in_window`/`drag-curve`=`describe` bounds→fraction→`perform`; delete `locator.{cpp,hpp}`; `src/input` → the raw facade only. Raw input keeps its own seat (canonical §5: explicit coordinates are a caller-owned fallback), NOT a second Session — no double connection.

## Wave 3 — Capture purge (`src/screen/` 4,876 → facade + relocations)

Prereq: W1. Port before delete:

- `x11_capture.cpp:1300` → the runtime-owned capture route is the only capturer; `Screen` becomes a facade over `Session::capture` (public `Image` results served from `Frame::image`; `Screen::window(uint32_t)` raw-XID overload deprecated → deleted here — migration row 3).
- `enumerate.cpp:858` → X11 tree/topology source (window/output enumeration already duplicated there).
- `record.cpp:1105` → re-hosted on the capture route + `TileDiffer` + a single pacing governor (the governor task lives here; kills scattered fps/poll constants).
- `workflow.cpp:453` (batch/watch/compare) → wait-engine predicates + `TileDiffer`; burns its allowlisted sleep; `InjectGate` acquired by both the capture route and pointer route (self-interference gate becomes real).
- `virtual_display.cpp:480` → **relocate** to the session/workspace domain (`src/session/`), unchanged behavior.

**Delete:** `src/screen/{x11_capture,enumerate,workflow,record,window_match}.*`; keep thin `screen.cpp` facade. Budget: `src/screen=4876` → `~600`.

## Wave 4 — Observation purge (`src/event/` 5,486 → 0; the big one)

Prereq: W1 (parallel with W2/W3/W5). Port before delete:

| Legacy | Destination |
|---|---|
| `xinput2.cpp:635`, `evdev.cpp:452` | runtime event sources under `drivers/desktop/x11/` + `drivers/device/evdev/`, with `enable/disable` driven by EventBus demand refcounts down to XI2 masks / reader threads (the refcount plumbing gains its consumer) |
| `window_x11.cpp:1005` | X11 tree/topology source deltas — `next_update` becomes real; focus/title/geometry changes arrive as graph deltas |
| `atspi.cpp:995` (monitor) | `AtspiRuntime` event source; `RegisterEvent` becomes demand-driven |
| `browser_bridge.cpp:640` | `src/drivers/semantic/webextension/` contributing document/tab nodes + `active_child` relation changes |
| `browser_classifier.hpp:145` | low-confidence evidence adapter feeding `TargetRegistry` candidate aliases — never identity (fixes invariants #1/#5/#8) |
| `state.cpp`+`state_source.cpp:488` | TreeStore/EWMH snapshot provider registered with the bus; fix `window.created` replay row `None` → `CurrentSet` (`event_descriptor.hpp:113-117`) |
| `platform_factory.cpp` + `source_registry.cpp` + `monitor_source.hpp` + `source.hpp` | die with the composition switch (daemon already on runtimes after W1) |
| `BrowserTab`/`BrowserTabSwitched` in `event.hpp:51,108-130` + descriptor row | **create `src/compat/eventgrab_v1/`**: v1 projections derived from `active_child.changed` + node properties; storage/codec re-pointed; core vocabulary browser-free |

Also here: JSONL storage schema upgraded to carry the envelope (origin/subject/cause/revisions/sequence — currently dropped at `jsonl_sink.cpp:330-336`); per-source sequence numbers.

**Delete:** entire `src/event/`; `include/grab/active_kind_probe.hpp` (re-point `event_descriptor.hpp` + `transport/service.cpp`); most `pid.hpp` consumers die. Budget lines `src/event`, and `src/platform` (relocated under `drivers/desktop/x11/` in this wave — XCB connection, XI seat, XKB keymap move verbatim) both deleted. Gate: v1 wire round-trip green **through compat**, demand-refcount ring-3 test proves XI2 masks toggle with subscriber count.

> **Wave-4 execution status (2026-07-17, branch `purge/wave4-exec` off `c17cc1c`).** T0 (`9b85ed2` session-scoped RuntimeId authority — realized as binding-level `subject.runtime` stamping, keeping each source's internal id for TreeStore restart detection) and T1 (`ce968c9` continuous `ObservationPump` — jthread per primary runtime looping `wait_for_event`→bus + demand-driven masks; AtSpi excluded because its event source is itself a bus subscriber) DONE. **T2 (daemon swap) hit a budget wall + RULING:** bus unification needs seams on `grab::Session` (frozen `src/core`, ~23 LOC headroom). **Ruling — pull the Wave-6 move forward:** `git mv src/core/session.cpp → src/kernel/lifecycle/session.cpp` (public header stays; frees ~297 LOC from the frozen scan; shrink the `src/core` budget same commit) [W4.2a], THEN add bus seams in the now-non-frozen file and do the daemon swap via option A (daemon points `sink_`/`TransportServer` at `session_->core().bus()`, starts the pump) [W4.2]. **Two landmines the orchestrator surfaced (honor in T2):** (1) storage drain thread must be stopped/joined BEFORE `session_.reset()` in daemon shutdown or the unified bus is use-after-free; (2) `ActiveKindProbe` is already probe-based — the swap just supplies a new probe impl sourced from runtime routes, no service/server/loopback signature changes.

## Wave 5 — Spine relocation (mechanical; parallel with W2–W4)

- `src/cli/` → `src/frontends/cli/`; `src/transport/` + `src/service/` → `src/frontends/grpc/` (§12 layout); pure `git mv` + include fixups.
- v1 wire mapping (`transport/codec.cpp` v1 sections + `proto_descriptor.hpp:62`) → `src/compat/eventgrab_v1/` (coordinate with W4, which creates the dir).
- One dispatcher: loopback and gRPC route through shared descriptor-driven handlers (removes the second dispatch path and the `SetClientContext` loopback asymmetry; makes the single-wrap CI test real instead of tautological — count handlers, not the constexpr array against itself).
- Fix en passant (audit §5): `terminate_owned()` actually calls `OwnedProcess::terminate(grace)` (`service.cpp:693-696`); tolerant unknown-event decode (skip + counter, not stream death).

## Wave 6 — Core dissolution + public-surface purge (last)

Prereq: W2–W5. `src/core/` (4,836 LOC) dissolves into named §12 concerns — no rewrite, `git mv` + namespace/include fixes:

| File | Destination |
|---|---|
| `reactor.cpp:690` | `src/kernel/scheduling/` |
| `event_bus.cpp:728` | `src/kernel/events/` (public `event_bus.hpp` internalized; `watch.hpp` from W1.2 is the public surface) |
| `resolver/registry/prober/doctor` (650) | `src/kernel/routing/` (resolver is the doctor's data source per WP13) |
| `context/id_factory/space_graph/process_ref/vendor_adapt` (1,350) | `src/kernel/{lifecycle,identity,presentation}/` per concern |
| `session.cpp:320` | `src/kernel/lifecycle/` (composition-root impl) |
| `log.hpp/ascii.hpp` (300) | `src/kernel/support/` (consumers are core+session only — verified 2026-07-16) |
| `permission/monitor` (300) | `src/spi/` (WP10 portal work will own them) |

Public header purge (allowlist shrinks to the §12 list + blessed WP0 additions `ids/context/space/origin/process_ref/trace/command_descriptor/event_descriptor/enum_table/geometry*/version`): **delete** `pid.hpp` (last consumers migrated to `BorrowedProcessId` — migration row 10), `event_bus.hpp`, `payload_fields.hpp` (internalized), `window_match.hpp`/`pointer_button.hpp`/`drag.hpp`/`keymap.hpp` per facade outcomes.

> **Correction (2026-07-17, after Wave-4 T2/Final):** `active_kind_probe.hpp` **stays** — it is NOT a deletable legacy header. The T2 daemon swap kept `grab::ActiveKindProbe` as the canonical live abstraction ("is this event kind actively produced") and gave it a new `RuntimeKindProbe` implementation (`daemon.cpp`); it drives `event_type_descriptors(const ActiveKindProbe*)` → `ListEventTypes` and is consumed by `transport/service`, `transport/server`, `client/loopback_transport`. The earlier "delete active_kind_probe" lines (Wave 4 Final and this Wave-6 list) are struck. Retiring it later would mean folding the active-kind query into the runtime/descriptor surface — a design change, tracked separately if ever wanted, not a mechanical header deletion.

**Final gate:** `src/{core,event,input,platform}` do not exist; `legacy_budget.txt` is empty and both it and its check are removed from the ratchet script (allowlist check stays permanently); `check_invariants.sh` gains the kernel-platform-free rule for the new subdirs; full suite green; authorship single-identity across the whole purge range.

## Explicitly out of scope (additions, not purge)

`facet.hpp`/`application.hpp` contracts, safety policy, `PinnedTarget`, provider pushdown, PixelDensity, Wayland/agent phases — tracked by the canonical plan, not this document. This plan only removes what the audit proved replaced-but-undeleted, and wires exactly as much as deletion requires.

## Self-review checklist (run at each wave close)

1. Every deleted file's behaviors enumerated in the wave plan with a replacement or waiver (G2).
2. `grep -rn '<deleted symbol>' src/ include/ tests/` → zero stragglers.
3. Ratchet + allowlist lowered in the same commit as the deletion (G5).
4. `ctest` green including `grab_invariant_checks`, `grab_legacy_ratchet`, v1 codec tests (G3/G4).
