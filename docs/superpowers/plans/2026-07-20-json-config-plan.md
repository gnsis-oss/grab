# JSON Configuration Implementation Plan

> **For agentic workers:** this plan is executed phase-by-phase by Codex
> (`codex exec`) runs driven from the orchestrating session. Each task is one
> TDD cycle ending in a commit. The normative semantics live in the spec:
> `docs/superpowers/specs/2026-07-20-json-config-design.md` — read it first;
> where this plan says "per spec §X" the spec section is the contract. Steps
> use checkbox (`- [ ]`) syntax for tracking.

**Goal:** config-driven `grab watch` (interval captures + mouse script +
daemon mode) and `grab batch` (app-launch targets + manifest + comparison)
from a JSON profile, per the 2026-07-20 spec rev 2.

**Architecture:** a pure `grab::config` module (nlohmann-parsed, typed,
strictly validated) feeding two engines: a per-config watcher thread
(deadline scheduler interleaving captures and `grab::Input` script steps,
rotation ledger) and a batch target runner (spawn → wait-for-window →
frames → manifest → compare). Platform seams added where review found gaps:
`OwnedProcess::wait`, query-tree enumeration fallback, capture-by-window-id,
RMSE + directory compare.

**Tech Stack:** C++23, nlohmann/json (already fetched), xcb, GTest/CTest,
Xvfb fixtures.

## Global Constraints

- C++23, Clang default toolchain, `-Wall -Wextra -Wconversion -Wshadow
  -Wpedantic -Werror`; ASan/UBSan default; clang-format + clang-tidy run on
  every build and must pass.
- Naming: CamelCase enum values, no `k` prefix on constants, trailing `_`
  on class members (public-data structs use bare names, matching
  `BatchItem`).
- **No raw sleeps under `src/`** — waits use timerfd/poll/reactor.
- JSON via nlohmann only; **no new external dependencies**.
- `[[nodiscard]]` on all queries and fallible operations; errors via
  `grab::Result<T>` (`grab::Error{ErrorCode, std::string message}`).
  Config errors use `ErrorCode::InvalidArgument` (validation) /
  `ErrorCode::NotFound` (missing file) with message format
  `"<file>:<json-pointer>: <reason>"`.
- Tests: named `constexpr` constants for every value — no magic numbers, no
  NOLINT suppressions; one test file per logical concern, registered in
  `tests/CMakeLists.txt` (unit tests join `grab_core_tests`; X-backed tests
  follow the existing Xvfb-fixture patterns there).
- New public header `include/grab/config.hpp` must be added to
  `tests/scripts/public_header_allowlist.txt`.
- Build/verify: `cmake --build build -j$(nproc)` then
  `ctest --test-dir build --output-on-failure` (scope with `-R` per task).
- Commits: one per task, message `feat(config): …` / `test(config): …`
  style; **user identity only — no Co-Authored-By, no AI attribution,
  no "Generated with" lines**.
- Do not modify anything outside the repo; do not touch `_legacy/` or
  `reference/`.

## File Structure

```
include/grab/config.hpp              public types + load/resolve
src/config/load.cpp                  parse boundary + decode + validation
src/config/pattern.{hpp,cpp}         filename pattern renderer
src/config/schedule.{hpp,cpp}        pure deadline scheduler (fake-clock testable)
src/config/rotation.{hpp,cpp}        rotation ledger (filesystem only)
src/config/environment.{hpp,cpp}     env overlay materializer
src/config/batch_manifest.{hpp,cpp}  BatchManifest read/write
src/image/compare_dirs.{hpp,cpp}     rmse + directory comparison
src/drivers/desktop/x11/config_watch.{hpp,cpp}   ConfigWatcher engine
src/drivers/desktop/x11/config_batch.{hpp,cpp}   run_config_batch
src/frontends/cli/watch_daemon.{hpp,cpp}         pid/status/daemonize/stop/status
src/frontends/cli/main.cpp           verb wiring (watch subverbs, batch --config)
profiles/watch-10s-notify.json, watch-1m-notify.json, batch-example.json
tests/config/test_{load,pattern,schedule,rotation,environment,batch_manifest,profiles}.cpp
tests/image/test_compare_dirs.cpp
tests/core/test_process_wait.cpp
tests/screen/test_enumerate_fallback.cpp, test_capture_window_id.cpp
tests/integration/test_config_watch.cpp, test_config_batch.cpp
tests/scripts/config_watch_smoke.sh, config_batch_smoke.sh
```

## Shared Interfaces (single source of truth for all tasks)

```cpp
// include/grab/config.hpp — namespace grab::config
enum class DisplayBackend : std::uint8_t { Native, Xvfb, Count };
enum class MatchKind      : std::uint8_t { Pid, WmClass, Title, WindowId, Count };
enum class StepAction     : std::uint8_t { Move, Click, ClickAt, Drag, Type, Key, Delay, Count };
enum class NotifyStrategy : std::uint8_t { Os, None, Count };
enum class CompareMode    : std::uint8_t { Exact, Rmse, Count };

struct DefaultsSection { std::string format{ "png" }; double timeout_s{ 15.0 };
                         bool kill_after{ true }; std::filesystem::path output_root; };
struct DisplaySection  { DisplayBackend backend{ DisplayBackend::Native };
                         std::uint16_t width{ 1920 }; std::uint16_t height{ 1080 };
                         std::uint8_t depth{ 24 }; };
struct TargetMatch     { MatchKind kind{ MatchKind::WmClass }; std::string text;
                         std::uint32_t pid{}; std::uint32_t window_id{}; };
struct WatchLimits     { std::uint64_t max_files{}; std::uint32_t max_age_days{};
                         std::uint64_t max_disk_mib{}; };   // 0 = unlimited
struct WatchSection    { std::uint32_t interval_ms{}; std::filesystem::path output;
                         std::string filename{ "capture_{timestamp}" };
                         std::string format{ "png" };
                         std::optional<TargetMatch> target; WatchLimits limits; };
struct ScriptStep      { StepAction action{ StepAction::Move };
                         std::int16_t x{}, y{};          // move/click_at; drag "from"
                         std::int16_t to_x{}, to_y{};    // drag "to"
                         std::uint8_t button{ 1 };       // click/click_at
                         std::string text;               // type: text, key: name
                         std::uint32_t delay_ms{}; };    // delay
struct ScriptSection   { bool loop{ false }; std::vector<ScriptStep> steps; };
struct TargetSpec      { std::string name; std::vector<std::string> argv;
                         std::vector<std::pair<std::string, std::string>> env;
                         MatchKind match{ MatchKind::Pid }; std::string pattern;
                         std::uint32_t frames{ 1 }; std::uint32_t interval_ms{ 200 };
                         std::uint32_t delay_ms{ 1000 }; double timeout_s{ 15.0 };
                         bool kill_after{ true }; };
struct BatchSection    { std::filesystem::path output_root; };
struct NotifySection   { bool enabled{ false };
                         NotifyStrategy strategy{ NotifyStrategy::Os };
                         std::uint32_t popup_timeout_ms{ 2000 }; };
struct CompareSection  { CompareMode mode{ CompareMode::Rmse }; double threshold{ 5.0 };
                         std::optional<std::filesystem::path> ref; };
struct Config { std::filesystem::path source; DefaultsSection defaults;
                DisplaySection display; std::optional<WatchSection> watch;
                std::optional<ScriptSection> script; std::vector<TargetSpec> targets;
                BatchSection batch; NotifySection notifications; CompareSection compare; };

[[nodiscard]] grab::Result<Config> load( const std::filesystem::path& path );
[[nodiscard]] grab::Result<std::vector<Config>>
resolve( std::span<const std::string_view> explicit_paths );  // $GRAB_CONFIG fallback
```

```cpp
// src/config/pattern.hpp — namespace grab::config
struct PatternContext { std::chrono::system_clock::time_point now; std::uint32_t seq; };
[[nodiscard]] grab::Result<std::string>
render_filename( std::string_view pattern, const PatternContext& ctx );
// {timestamp}->UTC YYYYMMDDTHHMMSS.mmm, {date}, {time}, {seq}->%05u; appends
// ".png" when absent; error on absolute paths, "..", or unknown {token}.
[[nodiscard]] bool matches_pattern( std::string_view pattern, std::string_view name );
// true iff `name` could have been rendered from `pattern` (rotation ownership).
```

```cpp
// src/config/schedule.hpp — namespace grab::config
enum class DueKind : std::uint8_t { Capture, Step, Idle, Count };
struct Due { DueKind kind{ DueKind::Idle }; std::size_t step_index{};
             std::chrono::steady_clock::time_point wake_at; };
class WatchSchedule {   // fixed-delay cadence; capture wins ties (spec: scheduler)
  public:
    WatchSchedule( std::chrono::milliseconds interval, const ScriptSection* script );
    [[nodiscard]] Due next( std::chrono::steady_clock::time_point now );
    void capture_done( std::chrono::steady_clock::time_point now );
    void step_done( std::chrono::steady_clock::time_point now );
    void fail_script();                                   // script stops, captures go on
    [[nodiscard]] std::uint64_t skipped_captures() const noexcept;
};
```

```cpp
// src/config/rotation.hpp — namespace grab::config
class RotationLedger {  // owns only pattern-matching files in `dir` (spec: rotation)
  public:
    RotationLedger( std::filesystem::path dir, std::string pattern, WatchLimits limits );
    [[nodiscard]] grab::Result<void> scan();              // startup adoption
    [[nodiscard]] grab::Result<void> adopt( const std::filesystem::path& file );
    [[nodiscard]] grab::Result<std::size_t>
    prune_age( std::chrono::system_clock::time_point now );
    [[nodiscard]] bool paused() const noexcept;           // ≥ cap; resumes < 90 %
    [[nodiscard]] grab::Result<void> refresh_disk();      // re-stat while paused
};
```

```cpp
// src/config/environment.hpp — namespace grab::config
[[nodiscard]] std::vector<std::string> overlay_environment(
    std::span<const std::pair<std::string, std::string>> overrides );
// full inherited environ as "K=V", overrides replacing/adding keys.
```

```cpp
// include/grab/process_ref.hpp — class OwnedProcess (addition)
[[nodiscard]] grab::Result<int> wait( std::chrono::milliseconds timeout );
// poll(pidfd) + waitid(P_PIDFD, WEXITED); returns exit status; Timeout-class
// ErrorCode on expiry (child still running).
```

```cpp
// src/drivers/desktop/x11/workflow.hpp — namespace grab::screen (additions)
[[nodiscard]] grab::Result<std::uint32_t>
resolve_target( grab::Screen& screen, const grab::config::TargetMatch& match );
[[nodiscard]] grab::Result<void>
capture_window_to( grab::Screen& screen, std::uint32_t window_id,
                   const std::string& out_path );
```

```cpp
// src/image/compare_dirs.hpp — namespace grab::image
enum class DirCompareMode : std::uint8_t { Exact, Rmse, Count };
struct FileCompareResult { std::string name; bool in_ref{}, in_current{};
                           double score{}; bool passed{}; };
[[nodiscard]] grab::Result<double> rmse( const Image& a, const Image& b );
[[nodiscard]] grab::Result<std::vector<FileCompareResult>>
compare_dirs( const std::filesystem::path& ref, const std::filesystem::path& current,
              DirCompareMode mode, double threshold );
```

```cpp
// src/config/batch_manifest.hpp — namespace grab::config
enum class RunState : std::uint8_t { Running, Done, Failed, Count };
struct TargetOutcome { std::string name; std::vector<std::string> argv;
                       std::int64_t pid{ -1 }; std::uint32_t window_id{};
                       std::vector<std::string> files; std::string error; };
struct FileCompareEntry { std::string name; double score{}; bool passed{}; };
struct BatchManifest {
    std::filesystem::path profile; std::string started_at, ended_at;
    RunState state{ RunState::Running };
    std::vector<TargetOutcome> targets; std::vector<FileCompareEntry> compare;
    [[nodiscard]] grab::Result<void>
    write( const std::filesystem::path& session_dir ) const;  // tmp + rename
    [[nodiscard]] static grab::Result<BatchManifest>
    read( const std::filesystem::path& session_dir );
};
```

```cpp
// src/drivers/desktop/x11/config_watch.hpp — namespace grab::screen
struct WatchStats { std::uint64_t captured{}, errors{}, skipped{};
                    bool paused{}, script_failed{}; std::string last_capture; };
class ConfigWatcher {
  public:
    [[nodiscard]] static grab::Result<ConfigWatcher>
    start( const grab::config::Config& cfg );   // display+target resolve, thread spawn
    void stop();                                // signal + join
    [[nodiscard]] WatchStats stats() const;     // thread-safe snapshot
};
```

```cpp
// src/frontends/cli/watch_daemon.hpp — namespace (anonymous per CLI style ok, or grab::cli)
struct DaemonPaths { std::filesystem::path pid_file, status_file, log_file;
                     [[nodiscard]] static DaemonPaths standard(); };  // $XDG_RUNTIME_DIR/grab/
[[nodiscard]] grab::Result<void> daemonize( const DaemonPaths& paths );
[[nodiscard]] grab::Result<void> write_status(
    const DaemonPaths& paths,
    std::span<const std::pair<std::string, grab::screen::WatchStats>> stats );
[[nodiscard]] int run_watch_stop( const DaemonPaths& paths );
[[nodiscard]] int run_watch_status( const DaemonPaths& paths, bool as_json );
```

```cpp
// src/drivers/desktop/x11/config_batch.hpp — namespace grab::screen
struct ConfigBatchResult { grab::config::BatchManifest manifest;
                           std::filesystem::path session_dir;
                           std::uint32_t target_errors{}, compare_failures{}; };
[[nodiscard]] grab::Result<ConfigBatchResult>
run_config_batch( const grab::config::Config& cfg, grab::notify::Notifier* notifier );
```

---

## Phase 1 — config core (no X dependencies)

### Task 1: parse boundary + typed structs

**Files:** Create `include/grab/config.hpp`, `src/config/load.cpp`,
`tests/config/test_load.cpp`; Modify `CMakeLists.txt` (add `src/config/*.cpp`
to `grab_core` sources, or a `grab_config` object lib folded into it,
matching how `src/session` is wired), `tests/CMakeLists.txt`,
`tests/scripts/public_header_allowlist.txt` (+`config.hpp`).

**Interfaces:** Produces `grab::config` structs + `load` (see Shared
Interfaces). Internal to `load.cpp`: `parse_document(path)` returning
`Result<nlohmann::json>` — comments rejected (nlohmann default), duplicate
keys and depth > 64 rejected via `json::parser_callback_t`, exceptions
converted at this boundary to `Error{InvalidArgument, "<file>: byte <n>: …"}`.

- [ ] Write failing tests: `test_load.cpp` —
  `RejectsComment`, `RejectsDuplicateKey` (`{"a":1,"a":2}`),
  `RejectsDepthAboveCap` (65 nested arrays), `AcceptsDepthAtCap` (64),
  `ParseErrorCarriesByteOffset`, `MissingFileIsNotFound`,
  `MinimalConfigLoads` (`{"schema_version":1}` → all defaults).
- [ ] `cmake --build build && ctest --test-dir build -R config` → FAIL
  (header/functions missing).
- [ ] Implement header + `parse_document` + minimal `load` (schema_version
  gate only).
- [ ] Tests pass; whole build green (format/tidy).
- [ ] Commit `feat(config): typed config structs and strict JSON parse boundary`.

### Task 2: decode, validation, defaults application, resolve

**Files:** Modify `src/config/load.cpp`; Test `tests/config/test_load.cpp`
(extend).

**Interfaces:** Completes `load` + `resolve` per the spec's normative field
table — every field type/range/default exactly as specified there.

- [ ] Failing tests (each asserts `ErrorCode` and that the message contains
  the expected JSON Pointer):
  `UnknownKeyIsError` (`/watch/intervall_ms`),
  `UnsupportedSchemaVersion` (2 → message says unsupported version, not
  unknown key), `IntegerFieldRejectsFloat` (`interval_ms: 1.0`),
  `TargetMatchExactlyOneKey` (two keys → error; zero keys → error),
  `WindowIdParsesHex` (`"0x1400003"` → `window_id == 0x1400003U`; bare
  `"1400003"` or malformed hex → error),
  `ScriptLoopRequiresDelay` (loop:true, no delay ≥ 100 ms total),
  `ArgvMustBeNonEmpty`, `TargetEnvDisplayForbiddenWithXvfb`,
  `PatternRequiredForWmClassMatch` / `PatternForbiddenForPidMatch`,
  `TildeExpandsToHome`, `RelativePathsResolveAgainstConfigDir`,
  `DefaultsApplyToTargets` (timeout_s/kill_after fallback, target wins),
  `OutputRootPrefixesRelativeOutput`,
  `DeferredBackendRejected` (`"weston"` → message contains "deferred"),
  `NotifyTruthTable` (enabled:false+os and enabled:true+none both valid),
  `ResolveUsesGrabConfigEnvWhenNoPaths` (setenv in test),
  `ResolveErrorsWithNoPathsAndNoEnv`, `WholeFileValidatesRegardlessOfVerb`
  (bad `targets` fails a watch-only load).
- [ ] Run → FAIL.
- [ ] Implement hand-written decoders per section; helper
  `decode_error(file, pointer, reason)`.
- [ ] Tests pass; build green.
- [ ] Commit `feat(config): full section decoding and validation`.

### Task 3: filename pattern renderer

**Files:** Create `src/config/pattern.{hpp,cpp}`,
`tests/config/test_pattern.cpp`.

- [ ] Failing tests: `TimestampHasMillisecondResolution` (fixed time_point →
  exact `20260720T101530.250` form), `SeqZeroPads` (`00042`),
  `AppendsPngWhenMissing`, `KeepsExplicitPng`, `RejectsUnknownToken`,
  `RejectsAbsolute` (`/etc/x`), `RejectsDotDot`,
  `MatchesPatternAcceptsRendered` (render then match → true),
  `MatchesPatternRejectsForeign` (`unrelated.txt` → false).
- [ ] Run → FAIL. Implement. Pass. Commit
  `feat(config): filename pattern renderer`.

### Task 4: watch schedule (pure)

**Files:** Create `src/config/schedule.{hpp,cpp}`,
`tests/config/test_schedule.cpp`.

Semantics per spec "Cadence" + "Script scheduler": fixed-delay (next capture
armed `interval` after `capture_done`), first capture due immediately,
capture wins equal deadlines, overrun deadlines are skipped once and
counted, steps advance sequentially, `loop` restarts the list,
`fail_script()` removes steps from scheduling.

- [ ] Failing tests (all with synthetic `steady_clock::time_point`s):
  `FirstCaptureDueImmediately`, `FixedDelayNotFixedRate` (slow capture →
  next deadline shifts), `CaptureWinsTie`, `StepsInterleave`,
  `OverrunSkipsAndCounts` (step_done far past two deadlines → one capture
  due, `skipped_captures()==1`... assert exact), `LoopRestartsSteps`,
  `NoLoopFinishesSteps` (then only captures due), `FailScriptStopsSteps`.
- [ ] Run → FAIL. Implement. Pass. Commit
  `feat(config): fixed-delay watch scheduler`.

### Task 5: rotation ledger

**Files:** Create `src/config/rotation.{hpp,cpp}`,
`tests/config/test_rotation.cpp` (tmpdir via
`std::filesystem::temp_directory_path`).

- [ ] Failing tests: `ScanAdoptsOnlyPatternMatches` (foreign file untouched
  by later rotation), `MaxFilesDeletesOldest` (mtime order),
  `PruneAgeRemovesOldLedgerFilesOnly`, `DiskCapPauses` (cap 1 MiB, write
  past it), `ResumesBelowNinetyPercent` (delete files, `refresh_disk()`,
  `paused()==false`), `SymlinksNeverDeleted`.
- [ ] Run → FAIL. Implement. Pass. Commit
  `feat(config): rotation ledger with ownership and hysteresis`.

### Task 6: environment overlay + shipped profiles

**Files:** Create `src/config/environment.{hpp,cpp}`,
`tests/config/test_environment.cpp`, `profiles/watch-10s-notify.json`,
`profiles/watch-1m-notify.json`, `profiles/batch-example.json`,
`tests/config/test_profiles.cpp`.

Profiles are the legacy TOML conversions per spec (10 s/60 s, output,
filename, limits, notifications) and a fresh batch example using `argv`.

- [ ] Failing tests: `OverlayKeepsInherited` (PATH present),
  `OverlayReplacesKey`, `OverlayAddsKey`;
  `test_profiles.cpp`: `AllShippedProfilesLoad` — iterates the three
  `profiles/*.json` (path via a compile definition
  `GRAB_PROFILES_DIR="${CMAKE_SOURCE_DIR}/profiles"`), asserts `load`
  succeeds and spot-checks `interval_ms == 10000` for the 10 s profile.
- [ ] Run → FAIL. Implement + author profiles. Pass. Commit
  `feat(config): environment overlay and shipped profiles`.

## Phase 2 — platform seams

### Task 7: `OwnedProcess::wait`

**Files:** Modify `include/grab/process_ref.hpp`,
`src/kernel/lifecycle/process_ref.cpp`; Test
`tests/core/test_process_wait.cpp`.

- [ ] Failing tests: `WaitReturnsExitStatus` (spawn `/bin/true` → 0;
  `/bin/false` → 1), `WaitTimesOutOnRunningChild` (spawn `/bin/sleep 30`,
  wait 50 ms → Timeout-class error, then `terminate` + wait succeeds),
  `WaitOnReapedChildErrors` (second wait → error, not hang).
- [ ] Run → FAIL. Implement via `poll` on the pidfd +
  `waitid(P_PIDFD, …, WEXITED)`. Pass. Commit
  `feat(core): OwnedProcess::wait with timeout`.

### Task 8: query-tree enumeration fallback

**Files:** Modify `src/drivers/desktop/x11/enumerate.cpp`; Test
`tests/screen/test_enumerate_fallback.cpp` (Xvfb fixture display, **no** WM,
no `_NET_CLIENT_LIST` stub).

Per spec "Window enumeration fallback": when `_NET_CLIENT_LIST` is absent
or empty, walk `xcb_query_tree` from the root, keep mapped,
non-override-redirect windows, read `WM_CLASS`, `_NET_WM_NAME`/`WM_NAME`,
`_NET_WM_PID` directly.

- [ ] Failing test: create a bare xcb window with a distinctive `WM_CLASS`
  on the fixture display, map + flush, enumerate → the window is listed
  with class, title, and pid populated; unmapped sibling is not listed.
- [ ] Run → FAIL. Implement fallback in the existing enumeration path
  (same return type; `_NET_CLIENT_LIST` still preferred when present).
  Pass — including the existing enumeration tests (no regressions).
- [ ] Commit `feat(screen): query-tree enumeration fallback for WM-less displays`.

### Task 9: target resolution + capture by window id

**Files:** Modify `src/drivers/desktop/x11/workflow.{hpp,cpp}`; Test
`tests/screen/test_capture_window_id.cpp`.

- [ ] Failing tests: `ResolveByWmClass`, `ResolveByTitleSubstring`,
  `ResolveByPid`, `ResolveByWindowId` (parses `0x…` string upstream —
  resolve receives the numeric id and verifies existence),
  `ResolveZeroMatchesIsNotFound`, `CaptureWindowIdWritesPng` (create +
  fill a window, capture to tmp path, decode PNG, assert dimensions).
- [ ] Run → FAIL. Implement using the enumeration data (Task 8) and the
  existing internal XComposite capture route used by `window_by_class`.
  Pass. Commit `feat(screen): target resolution and capture by window id`.

### Task 10: RMSE + directory comparison

**Files:** Create `src/image/compare_dirs.{hpp,cpp}`; Test
`tests/image/test_compare_dirs.cpp`.

- [ ] Failing tests: `RmseZeroForIdentical`, `RmseKnownValue` (two 2×1
  images with channel delta 10 → expected RMSE computed in the test as a
  named constant), `RmseSizeMismatchErrors`;
  `CompareDirsPairsByName`, `MissingInCurrentFails`, `ExtraInCurrentFails`
  (both directions produce a failed `FileCompareResult` with the
  `in_ref`/`in_current` flags telling which), `ExactModeZeroDiff`,
  `RmseModeThreshold` (score just under/over threshold).
- [ ] Run → FAIL. Implement (PNG decode via the in-tree codec). Pass.
  Commit `feat(image): rmse metric and directory comparison`.

### Task 11: BatchManifest

**Files:** Create `src/config/batch_manifest.{hpp,cpp}`; Test
`tests/config/test_batch_manifest.cpp`.

- [ ] Failing tests: `WriteThenReadRoundTrips` (all fields),
  `WriteIsAtomic` (no `manifest.json.tmp` left behind; content valid after
  overwrite), `CrashTell` (write with `state: Running`, read → state is
  `Running` — the crashed-run marker), `ReadMissingManifestIsNotFound`.
- [ ] Run → FAIL. Implement (nlohmann serialize, `manifest.json.tmp` +
  `std::filesystem::rename`). Pass. Commit
  `feat(config): batch manifest with atomic writes`.

## Phase 3 — watch engine + CLI

### Task 12: ConfigWatcher engine

**Files:** Create `src/drivers/desktop/x11/config_watch.{hpp,cpp}`; Test
`tests/integration/test_config_watch.cpp` (Xvfb fixture).

Engine loop per spec: own thread; timerfd wait until `WatchSchedule::next`;
capture via Task 9 (or full display when no target); write via Task 3
pattern + Task 5 ledger; script steps via one `grab::Input` connection
(action→call mapping: Move→`move`, Click→`click`, ClickAt→`click_at`,
Drag→`drag`, Type→`type_text`, Key→`press_key`, Delay→timerfd wait);
error policy per spec (tick errors counted + continue, script failure →
`fail_script`, notify failures logged once); `display.backend == Xvfb`
starts a `VirtualDisplay` owned by the watcher.

- [ ] Failing integration test: config with `interval_ms` = 100, tmp
  output, no target (full display), script = move/move/delay(50) loop —
  run 1 s, `stop()` — assert ≥ 5 files, all matching the pattern,
  `stats().captured` equals file count, `script_failed == false`;
  second test with `max_files` = 3 asserts rotation held at 3.
- [ ] Run → FAIL. Implement. Pass. Commit
  `feat(screen): config-driven watch engine with script scheduler`.

### Task 13: watch CLI surface + daemon mode

**Files:** Create `src/frontends/cli/watch_daemon.{hpp,cpp}`; Modify
`src/frontends/cli/main.cpp` (subverb parsing: `watch start CONFIG...
[--daemon] [--interval MS] [--output DIR]`, `watch stop`,
`watch status [--json]`; legacy `watch --window/--out` preserved when the
first arg starts with `-`); Test `tests/cli/watch_daemon_test.cpp` (unit:
pid/status file handling against tmp `DaemonPaths`, stale-pid detection
via a known-dead pid), `tests/scripts/config_watch_smoke.sh` + CTest
registration.

- [ ] Failing unit tests: `StatusRoundTrips`, `StalePidReported`,
  `SecondStartWithLivePidFails` (live pid = test's own `getpid()`).
- [ ] Implement daemon utils (double-fork/setsid, stdio → log file,
  atomic status rewrite each tick) + CLI wiring with overrides applied to
  every loaded config.
- [ ] Smoke script (self-provisioned Xvfb, pattern of
  `examples/event_logger_smoke.sh`): foreground run captures ≥ 3 files;
  `--daemon` then `status --json` shows the config and live pid; `stop`
  terminates within 10 s and exits 0.
- [ ] All green (unit + smoke via ctest). Commit
  `feat(cli): watch start/stop/status with daemon mode`.

## Phase 4 — batch runner + CLI

### Task 14: run_config_batch + CLI wiring

**Files:** Create `src/drivers/desktop/x11/config_batch.{hpp,cpp}`; Modify
`src/frontends/cli/main.cpp` (`batch --config PATH` branch; existing
simple batch preserved); Test `tests/integration/test_config_batch.cpp`,
`tests/scripts/config_batch_smoke.sh` + CTest registration.

Runner per spec batch steps 1–5: virtual display, session dir
(`_2` suffix on collision), manifest after every target, env overlay
(Task 6) + DISPLAY forced, spawn → `resolve_target` poll loop bounded by
`timeout_s` (uses Task 7 `wait` with zero-ish timeout to detect early
child exit → target error), settle via timerfd, frames loop, per-frame
notify, kill_after (SIGTERM → 2 s → SIGKILL → `wait`), compare via
Task 10 into manifest, exit code rule.

- [ ] Failing integration test: config with one target whose argv is a
  tiny helper X client (reuse the window helper pattern from
  `tests/screen`; if none is reusable, add `tests/fake/config_batch_window.cpp`
  — bare xcb window with fixed WM_CLASS that maps and sleeps), match
  `wm_class`, `frames` = 2 → session dir exists, 2 PNGs, manifest
  `state == Done`, target outcome has pid/window/files; second test:
  argv = `/bin/false` → manifest `state == Failed` tellingly, target error
  recorded, exit path returns nonzero; third: compare against a ref dir
  with one mismatching + one missing file → both failures in manifest.
- [ ] Run → FAIL. Implement runner + CLI. Pass.
- [ ] Smoke: batch profile against Xvfb end-to-end, asserts manifest and
  exit code.
- [ ] Commit `feat(cli): config-driven batch runner with manifest and compare`.

### Task 15: full verification pass

- [ ] `cmake --build build -j$(nproc)` — zero warnings (they're errors),
  clang-format/tidy clean.
- [ ] `ctest --test-dir build --output-on-failure` — entire suite, not
  just new tests; fix any regressions.
- [ ] `grab watch start profiles/watch-10s-notify.json` sanity run on a
  fixture display; `grab batch --config profiles/batch-example.json`.
- [ ] Update the CLI usage text in `main.cpp` (`print_usage`) with the new
  verbs; commit `docs(cli): usage for config-driven watch and batch`.
