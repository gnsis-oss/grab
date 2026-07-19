# Event Logger Example Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A live terminal feed of every event grab observes (`timestamp -> category event -> description`), with a best-effort JSONL recording, plus a small library pre-fix making `event.timestamp` uniformly epoch seconds.

**Architecture:** One self-contained example (`examples/event_logger.cpp`) opens a `grab::Session`, subscribes to all event kinds on the session bus, and additionally starts `WindowTracker` and `BrowserBridge` producers on the session's public `reactor()`/`bus()` seam. A notify→post→drain pump consumes on the session reactor: mouse moves coalesce into ~300 ms summary lines, everything else prints one line, and every surviving event is written to a `JsonlSink`. Two library commits first make timestamps a real epoch-seconds invariant.

**Tech Stack:** C++23, XCB/XInput2 (existing drivers), nlohmann_json (public on `grab_core`), GTest, Xvfb + xdotool + python3 for the smoke script.

**Spec:** `docs/superpowers/specs/2026-07-19-event-logger-example-design.md` (approved). Read it before starting.

## Global Constraints

- Work in the `.worktrees/integrate` worktree, branch `feat/grab-port`. All paths below are relative to the worktree root.
- C++23, Clang default preset; warnings-as-errors; ASan/UBSan on by default; clang-format + clang-tidy run on every build — a task is not done until `cmake --build build` is warning-clean.
- Naming: CamelCase constexpr constants (no `k` prefix), member variables with trailing `_`.
- Tests: named `constexpr` constants for ALL test values — no magic numbers, no NOLINT suppressions for them.
- Examples are gated by `GRAB_BUILD_EXAMPLES` (default `ON`); they carry no GTest suite — their test cycle is build + scripted smoke.
- Commits: user's git identity only. No `Co-Authored-By`, no AI attribution, in any commit.
- Build/test commands used throughout:
  - Configure (once, if `build/` is stale): `cmake --preset default`
  - Build: `cmake --build build -j$(nproc)`
  - Library tests: `ctest --test-dir build -R <regex> --output-on-failure`
  - X11-gated tests need a display: run under `Xvfb :97 -noreset -screen 0 1280x800x24 &` with `DISPLAY=:97` (they `GTEST_SKIP` without one).

---

### Task 1: Library pre-fix — X11 input events stamp epoch seconds

X11 input events currently stamp `raw.time` (an X-server milliseconds counter); the window tracker and AT-SPI stamp `system_clock` epoch seconds. This task introduces one shared internal helper and converts the X11 event source to it.

**Files:**
- Create: `src/kernel/events/wall_clock.hpp`
- Modify: `src/drivers/desktop/x11/x11_event_source.cpp` (4 stamping sites: lines 196, 212, 298, 308)
- Modify: `src/drivers/desktop/x11/window_tracker.cpp` (replace its private `now_timestamp_s()` with the shared helper)
- Test: `tests/drivers/x11/test_x11_event_source.cpp` (extend `TEST( X11EventSource, EnableInjectWaitYieldsInjectedSelfKeyDown )`)

**Interfaces:**
- Consumes: nothing new.
- Produces: `grab::kernel::now_timestamp_s()` — `[[nodiscard]] inline double`, wall-clock epoch **seconds** as `double`. Tasks 2–8 rely on `event.timestamp` being epoch seconds.

- [ ] **Step 1: Write the failing assertion**

In `tests/drivers/x11/test_x11_event_source.cpp`, inside `TEST( X11EventSource, EnableInjectWaitYieldsInjectedSelfKeyDown )`, right after the existing `ASSERT_NE( injected_key_down, events.end() );`, add:

```cpp
    // Timestamps are wall-clock epoch seconds (spec: uniform event clock),
    // not the X server's millisecond counter.
    const double now_s = std::chrono::duration<double>(
                             std::chrono::system_clock::now().time_since_epoch() )
                             .count();
    EXPECT_GT( injected_key_down->timestamp, now_s - timestampSlackSeconds );
    EXPECT_LT( injected_key_down->timestamp, now_s + timestampSlackSeconds );
```

Add to the test file's anonymous-namespace constants:

```cpp
    constexpr double timestampSlackSeconds = 60.0;
```

Add `#include <chrono>` to the test's includes if absent.

- [ ] **Step 2: Run test to verify it fails**

```bash
Xvfb :97 -noreset -screen 0 1280x800x24 & XVFB_PID=$!
cmake --build build -j$(nproc)
DISPLAY=:97 ctest --test-dir build -R X11EventSource --output-on-failure
```

Expected: `EnableInjectWaitYieldsInjectedSelfKeyDown` FAILS on the new `EXPECT_GT` (an X-server millisecond tick is nowhere near epoch seconds). If it SKIPs, the display isn't reaching the test — fix that before proceeding.

- [ ] **Step 3: Create the shared helper**

Create `src/kernel/events/wall_clock.hpp`:

```cpp
#pragma once

#include <chrono>

namespace grab::kernel
{

    // Uniform event-timestamp clock: wall-clock epoch seconds as double.
    // Every producer that stamps grab::Event::timestamp must use this.
    [[nodiscard]]
    inline double
    now_timestamp_s()
    {
        const auto duration = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration<double>( duration ).count();
    }

}    // namespace grab::kernel
```

- [ ] **Step 4: Convert the four X11 stamping sites**

In `src/drivers/desktop/x11/x11_event_source.cpp`, add `#include "kernel/events/wall_clock.hpp"` and replace every `.timestamp = static_cast<double>( raw.time )` / `static_cast<double>( raw.time )` motion-caller argument with `grab::kernel::now_timestamp_s()`:

- `make_key_event` (line ~196): `.timestamp = grab::kernel::now_timestamp_s(),`
- `make_button_event` (line ~212): `.timestamp = grab::kernel::now_timestamp_s(),`
- the two `append_motion_*` call sites (lines ~298 and ~308) currently passing `static_cast<double>( raw.time )`: pass `grab::kernel::now_timestamp_s()` instead.

If `raw.time` becomes unused in a function after this, remove the now-dead use, not the parameter of the XCB struct.

- [ ] **Step 5: Deduplicate window_tracker's helper**

In `src/drivers/desktop/x11/window_tracker.cpp`: delete its private `now_timestamp_s()` (lines ~82–89), add `#include "kernel/events/wall_clock.hpp"`, and point existing callers at `grab::kernel::now_timestamp_s()`.

- [ ] **Step 6: Run tests to verify green**

```bash
cmake --build build -j$(nproc)
DISPLAY=:97 ctest --test-dir build -R "X11EventSource|WindowTracker" --output-on-failure
```

Expected: PASS (no skips under the Xvfb display). Also run the full suite once — `ctest --test-dir build --output-on-failure` — other tests may have implicitly depended on `raw.time`; fix any that assert server-tick timestamps by updating them to epoch-plausibility assertions in the same style as Step 1.

- [ ] **Step 7: Commit**

```bash
git add src/kernel/events/wall_clock.hpp src/drivers/desktop/x11/x11_event_source.cpp src/drivers/desktop/x11/window_tracker.cpp tests/drivers/x11/test_x11_event_source.cpp
git commit -m "fix: stamp X11 input events with wall-clock epoch seconds"
```

---

### Task 2: Library pre-fix — graph translation and browser bridge stamp epoch seconds

Graph events publish with `timestamp` unset (0.0); browser frames without a `timestamp` field default to 0.0. Both misroute `JsonlSink`'s daily files and render as 1970.

**Files:**
- Modify: `src/kernel/lifecycle/session_impl.cpp` (`SessionCore::publish_tree_event`, `make_event` lambda at line ~730 — and any other `Event{...}` construction in that function missing `.timestamp`)
- Modify: `src/drivers/semantic/webextension/browser_bridge.cpp` (`make_integration_event`, line ~229: `.value_or( 0.0 )`)
- Test: `tests/kernel/lifecycle/test_session_core.cpp` (extend `TEST( SessionCore, AttachedFakeRuntimeSnapshotReachesStoreAndBus )`)
- Test: `tests/drivers/semantic/webextension/test_browser_bridge.cpp` (extend `TEST( BrowserBridge, ParsesTabSwitchedMessage )`)

**Interfaces:**
- Consumes: `grab::kernel::now_timestamp_s()` from Task 1.
- Produces: the epoch-seconds invariant now holds for every current producer.

- [ ] **Step 1: Write the failing assertions**

In `tests/kernel/lifecycle/test_session_core.cpp`, `TEST( SessionCore, AttachedFakeRuntimeSnapshotReachesStoreAndBus )`, after the existing `EXPECT_EQ( event->kind, grab::EventKind::NodeAdded );` add:

```cpp
    const double now_s = std::chrono::duration<double>(
                             std::chrono::system_clock::now().time_since_epoch() )
                             .count();
    EXPECT_GT( event->timestamp, now_s - timestampSlackSeconds );
    EXPECT_LT( event->timestamp, now_s + timestampSlackSeconds );
```

In `tests/drivers/semantic/webextension/test_browser_bridge.cpp`, `TEST( BrowserBridge, ParsesTabSwitchedMessage )` (line ~308) — `tabSwitchedJson` carries no `timestamp` key — assert the parsed event is stamped with arrival time:

```cpp
    const double now_s = std::chrono::duration<double>(
                             std::chrono::system_clock::now().time_since_epoch() )
                             .count();
    EXPECT_GT( event->timestamp, now_s - timestampSlackSeconds );
    EXPECT_LT( event->timestamp, now_s + timestampSlackSeconds );
```

Add to each file's anonymous-namespace constants (and `#include <chrono>` where absent):

```cpp
    constexpr double timestampSlackSeconds = 60.0;
```

(Adjust the dereference to the test's local variable: the session-core test's event is `event->`, the bridge test's `parse_browser_message` result is `event->` after `ASSERT_TRUE( event.has_value() )`.)

- [ ] **Step 2: Run tests to verify they fail**

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -R "SessionCore.AttachedFakeRuntimeSnapshotReachesStoreAndBus|BrowserBridge.ParsesTabSwitchedMessage" --output-on-failure
```

Expected: both FAIL on `EXPECT_GT` (timestamp is 0.0).

- [ ] **Step 3: Stamp graph translation**

In `src/kernel/lifecycle/session_impl.cpp`: add `#include "kernel/events/wall_clock.hpp"`; in `SessionCore::publish_tree_event`'s `make_event` lambda, add as the first designated initializer:

```cpp
            return Event{
                .timestamp = grab::kernel::now_timestamp_s(),
                .kind      = kind,
                ...
```

Then scan the rest of `publish_tree_event` (and the file) for any other `Event{` construction without `.timestamp` and stamp those identically.

- [ ] **Step 4: Stamp browser-bridge fallback**

In `src/drivers/semantic/webextension/browser_bridge.cpp`: add `#include "kernel/events/wall_clock.hpp"`; in `make_integration_event` change

```cpp
                .timestamp = timestamp_field( object ).value_or( 0.0 ),
```

to

```cpp
                .timestamp =
                    timestamp_field( object ).value_or( grab::kernel::now_timestamp_s() ),
```

- [ ] **Step 5: Run tests to verify green, then full suite**

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -R "SessionCore|BrowserBridge" --output-on-failure
ctest --test-dir build --output-on-failure
```

Expected: all PASS. Any test asserting `timestamp == 0` for these producers gets updated to the plausibility style.

- [ ] **Step 6: Commit**

```bash
git add src/kernel/lifecycle/session_impl.cpp src/drivers/semantic/webextension/browser_bridge.cpp tests/kernel/lifecycle/test_session_core.cpp tests/drivers/semantic/webextension/test_browser_bridge.cpp
git commit -m "fix: stamp graph and browser events with epoch seconds"
```

---

### Task 3: event_logger v1 — session, signals, subscription, pump, full-vocabulary formatter

Smallest runnable feed: open a session, subscribe to everything, print one line per event. No coalescer, no JSONL, no tracker/bridge yet (those are Tasks 4–7).

**Files:**
- Create: `examples/event_logger.cpp`
- Modify: `examples/CMakeLists.txt`

**Interfaces:**
- Consumes: `grab::Session::open( SessionOptions{} )` → `Result<std::unique_ptr<Session>>`; `session->watch( SubscriptionScope, QueueOptions )` → `Result<Subscription>`; `Subscription::set_notify( std::function<void()> )` / `try_pop_item()` → `std::optional<SubscriptionEvent>`; `session->post( std::function<void()> )` → `Result<void>`; `start_observation()`, `stop_observation()`, `close()`; `grab::category_of( EventKind )`, `grab::wire_name( EventKind )` from `grab/event_descriptor.hpp`.
- Produces (for Tasks 4–7): `format_timestamp( double ) -> std::string`; `category_label( EventKind ) -> std::string_view`; `feed_line( double, EventKind, const std::string& ) -> std::string`; `describe( const grab::Event& ) -> std::string`; `class EventLogger` with `void consume( const grab::SubscriptionEvent& )`, `void flush_pending()`, `std::size_t observed() const`, `std::size_t printed() const`, `std::size_t gaps() const`, `std::optional<grab::Error> error() const`; `class LogPump` with `install()` and `stop()` (tail-draining).

- [ ] **Step 1: Add the executable to the examples build**

Append to `examples/CMakeLists.txt`:

```cmake
add_executable(event_logger event_logger.cpp)
target_link_libraries(event_logger PRIVATE
    grab_core
    grab_kernel
    grab_session
    grab_platform_x11
    grab_driver_x11
    grab_driver_webextension
    grab_storage)
target_compile_features(event_logger PRIVATE cxx_std_23)
```

(`grab_driver_x11`, `grab_driver_webextension`, `grab_storage` are used from Tasks 5–7; linking them now keeps this file stable.)

- [ ] **Step 2: Write `examples/event_logger.cpp`**

```cpp
// event_logger — live terminal feed of everything grab observes.
//
//   ./event_logger [recording-dir] [--socket <path>]
//
// One line per event: "HH:MM:SS.mmm -> <category> event -> <description>".
// Runs until Ctrl+C. Later tasks add mouse-move coalescing, JSONL
// recording, window tracking, and a browser-bridge socket.

#include "grab/event.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "grab/watch.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <expected>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace
{

    constexpr std::size_t   queueCapacity      = 8'192U;
    constexpr int           signalSuccess      = 0;
    constexpr long          pollTickNanos      = 300'000'000L;    // 300 ms
    constexpr int           labelWidth         = 7;               // "browser"
    constexpr double        millisPerSecond    = 1'000.0;

    // ---- formatting -----------------------------------------------------

    [[nodiscard]]
    std::string
    format_timestamp( double epoch_s )
    {
        const auto seconds = static_cast<std::time_t>( epoch_s );
        const auto millis  = static_cast<int>(
            ( epoch_s - std::floor( epoch_s ) ) * millisPerSecond );
        std::tm local{};
        ::localtime_r( &seconds, &local );
        char buffer[ 16 ];
        std::snprintf( buffer,
                       sizeof( buffer ),
                       "%02d:%02d:%02d.%03d",
                       local.tm_hour,
                       local.tm_min,
                       local.tm_sec,
                       millis );
        return std::string{ buffer };
    }

    [[nodiscard]]
    std::string_view
    category_label( grab::EventKind kind )
    {
        switch( kind )
        {
            case grab::EventKind::NodeAdded:
            case grab::EventKind::NodeRemoved:
            case grab::EventKind::NodeChanged:
            case grab::EventKind::RelationAdded:
            case grab::EventKind::RelationRemoved:
            case grab::EventKind::ActiveChildChanged:
                return "ui";
            default:
                break;
        }
        switch( grab::category_of( kind ) )
        {
            case grab::EventCategory::Input: return "input";
            case grab::EventCategory::Window: return "os";
            case grab::EventCategory::Accessibility: return "a11y";
            case grab::EventCategory::Integration: return "browser";
            case grab::EventCategory::State: return "state";
            default: return "event";
        }
    }

    [[nodiscard]]
    std::string
    feed_line( double            timestamp,
               grab::EventKind   kind,
               const std::string& description )
    {
        std::ostringstream line;
        line << format_timestamp( timestamp ) << " -> ";
        const std::string_view label = category_label( kind );
        line << label;
        for( int pad = static_cast<int>( label.size() ); pad < labelWidth; ++pad )
        {
            line << ' ';
        }
        line << " event -> " << description;
        return std::move( line ).str();
    }

    [[nodiscard]]
    std::string
    quoted_or_number( const std::string& name,
                      std::uint32_t      code,
                      char               fallback_prefix )
    {
        if( !name.empty() )
        {
            return "\"" + name + "\"";
        }
        return std::string{ fallback_prefix } + std::to_string( code );
    }

    [[nodiscard]]
    std::string
    describe( const grab::Event& event )
    {
        using Kind = grab::EventKind;
        switch( event.kind )
        {
            case Kind::KeyDown:
            case Kind::KeyUp:
            {
                const auto* key = std::get_if<grab::InputKey>( &event.payload );
                if( key == nullptr )
                {
                    break;
                }
                return "key " + quoted_or_number( key->name, key->code, '#' ) +
                       ( event.kind == Kind::KeyDown ? " pressed" : " released" );
            }
            case Kind::KeyCombo:
            {
                const auto* combo = std::get_if<grab::KeyCombo>( &event.payload );
                if( combo == nullptr )
                {
                    break;
                }
                return "combo \"" + combo->text + "\" pressed";
            }
            case Kind::MouseClick:
            {
                const auto* click = std::get_if<grab::MouseClick>( &event.payload );
                if( click == nullptr )
                {
                    break;
                }
                return "button " +
                       quoted_or_number( click->name, click->button, '#' ) +
                       " clicked";
            }
            case Kind::MouseMove:
            {
                const auto* move = std::get_if<grab::MouseMove>( &event.payload );
                if( move == nullptr )
                {
                    break;
                }
                if( move->position.has_value() )
                {
                    return "pointer moved to (" +
                           std::to_string(
                               static_cast<std::int64_t>( move->position->x ) ) +
                           ", " +
                           std::to_string(
                               static_cast<std::int64_t>( move->position->y ) ) +
                           ")";
                }
                return "pointer moved (" + move->axis + ")";
            }
            case Kind::IdleStart:
            case Kind::IdleEnd:
            {
                const auto* idle = std::get_if<grab::Idle>( &event.payload );
                if( idle == nullptr )
                {
                    break;
                }
                char detail[ 32 ];
                std::snprintf( detail, sizeof( detail ), "%.1fs", idle->idle_s );
                return event.kind == Kind::IdleStart
                         ? "idle started (" + std::string{ detail } + ")"
                         : "idle ended (" + std::string{ detail } + ")";
            }
            case Kind::WindowFocusChanged:
            case Kind::WindowTitleChanged:
            case Kind::WindowCreated:
            case Kind::WindowClosed:
            {
                const auto* change = std::get_if<grab::WindowChange>( &event.payload );
                if( change == nullptr )
                {
                    break;
                }
                if( event.kind == Kind::WindowFocusChanged )
                {
                    return "window \"" + change->title + "\" (" + change->app +
                           ") focused";
                }
                if( event.kind == Kind::WindowTitleChanged )
                {
                    return "window title changed \"" + change->prev_title +
                           "\" to \"" + change->title + "\" (" + change->app + ")";
                }
                if( event.kind == Kind::WindowCreated )
                {
                    return "application \"" + change->app + "\" opened window \"" +
                           change->title + "\"";
                }
                std::string closed = "application \"" + change->app +
                                     "\" closed window \"" + change->title + "\"";
                if( change->duration_s > 0.0 )
                {
                    char detail[ 32 ];
                    std::snprintf( detail,
                                   sizeof( detail ),
                                   " (%.1fs)",
                                   change->duration_s );
                    closed += detail;
                }
                return closed;
            }
            case Kind::A11yButtonClicked:
            case Kind::A11yMenuOpened:
            case Kind::A11yMenuClosed:
            case Kind::A11yFocusChanged:
            case Kind::A11yTextChanged:
            case Kind::A11yStateChanged:
            {
                const auto* a11y = std::get_if<grab::A11yEvent>( &event.payload );
                if( a11y == nullptr )
                {
                    break;
                }
                switch( event.kind )
                {
                    case Kind::A11yButtonClicked:
                        return "button \"" + a11y->name + "\" clicked in \"" +
                               a11y->app + "\"";
                    case Kind::A11yMenuOpened:
                        return "menu \"" + a11y->name + "\" opened in \"" +
                               a11y->app + "\"";
                    case Kind::A11yMenuClosed:
                        return "menu \"" + a11y->name + "\" closed in \"" +
                               a11y->app + "\"";
                    case Kind::A11yFocusChanged:
                        return a11y->role + " \"" + a11y->name + "\" focused in \"" +
                               a11y->app + "\"";
                    case Kind::A11yTextChanged:
                        return "text changed in " + a11y->role + " \"" +
                               a11y->name + "\" (" + a11y->app + ")";
                    default:
                        return a11y->role + " \"" + a11y->name + "\" state: " +
                               a11y->detail + " (" + a11y->app + ")";
                }
            }
            case Kind::AppTabChanged:
            case Kind::AppContextUpdate:
            {
                const auto* app =
                    std::get_if<grab::IntegrationEvent>( &event.payload );
                if( app == nullptr )
                {
                    break;
                }
                if( event.kind == Kind::AppTabChanged )
                {
                    return "tab \"" + app->title + "\" focused";
                }
                return "context updated: \"" + app->detail + "\"";
            }
            case Kind::StateSnapshot:
            {
                const auto* snapshot =
                    std::get_if<grab::StateSnapshot>( &event.payload );
                if( snapshot == nullptr )
                {
                    break;
                }
                char detail[ 32 ];
                std::snprintf( detail,
                               sizeof( detail ),
                               "%.1f KB",
                               static_cast<double>( snapshot->json.size() ) /
                                   1'024.0 );
                return "snapshot (" + std::string{ detail } + ")";
            }
            case Kind::NodeAdded:
            case Kind::NodeRemoved:
            case Kind::NodeChanged:
            case Kind::RelationAdded:
            case Kind::RelationRemoved:
            case Kind::ActiveChildChanged:
            {
                const auto* graph = std::get_if<grab::GraphChange>( &event.payload );
                if( graph == nullptr )
                {
                    break;
                }
                switch( event.kind )
                {
                    case Kind::NodeAdded:
                        return "node added #" + std::to_string( graph->node );
                    case Kind::NodeRemoved:
                        return "node removed #" + std::to_string( graph->node );
                    case Kind::NodeChanged:
                        return "node changed #" + std::to_string( graph->node );
                    case Kind::RelationAdded:
                        return "relation added #" + std::to_string( graph->node ) +
                               " -> #" + std::to_string( graph->related );
                    case Kind::RelationRemoved:
                        return "relation removed #" + std::to_string( graph->node ) +
                               " -> #" + std::to_string( graph->related );
                    default:
                        return "active child changed #" +
                               std::to_string( graph->previous_active ) + " -> #" +
                               std::to_string( graph->node );
                }
            }
            default:
                break;
        }
        // Unknown kind or payload mismatch: fall back to the single-source
        // wire name so nothing is silently dropped.
        return std::string{ grab::wire_name( event.kind ) };
    }

    // ---- consumer -------------------------------------------------------

    class EventLogger
    {
        public:

            // Runs on the session reactor thread only.
            void
            consume( const grab::SubscriptionEvent& item )
            {
                if( const auto* gap = std::get_if<grab::QueueGapMarker>( &item ) )
                {
                    ++gaps_;
                    print( "!! gap: events dropped after seq " +
                           std::to_string( gap->last_delivered_sequence ) );
                    return;
                }
                const auto& event = std::get<grab::Event>( item );
                ++observed_;
                print( feed_line( event.timestamp, event.kind, describe( event ) ) );
            }

            void
            flush_pending()
            {
                // Coalescer arrives in Task 4.
            }

            void
            print( const std::string& line )
            {
                ++printed_;
                std::cout << line << '\n';
                std::cout.flush();
            }

            [[nodiscard]] std::size_t
            observed() const noexcept
            {
                return observed_;
            }

            [[nodiscard]] std::size_t
            printed() const noexcept
            {
                return printed_;
            }

            [[nodiscard]] std::size_t
            gaps() const noexcept
            {
                return gaps_;
            }

            void
            remember_error( grab::Error error )
            {
                const std::scoped_lock lock{ error_mutex_ };
                if( !error_.has_value() )
                {
                    error_ = std::move( error );
                }
            }

            [[nodiscard]]
            std::optional<grab::Error>
            error() const
            {
                const std::scoped_lock lock{ error_mutex_ };
                return error_;
            }

        private:

            std::size_t                observed_ = 0U;
            std::size_t                printed_  = 0U;
            std::size_t                gaps_     = 0U;
            mutable std::mutex         error_mutex_;
            std::optional<grab::Error> error_;
    };

    // ---- pump -----------------------------------------------------------

    // Notify -> post -> drain on the session reactor, with a tail-draining
    // stop: the final posted job drains the queue to exhaustion, flushes the
    // coalescer, and fulfils the fence promise, so the last events before
    // Ctrl+C are printed (the fence alone would not drain them).
    class LogPump
    {
        public:

            LogPump( grab::Session&     session,
                     grab::Subscription subscription,
                     EventLogger&       logger ) :
                session_{ &session },
                subscription_{ std::move( subscription ) },
                logger_{ &logger }
            {
            }

            void
            install()
            {
                subscription_.set_notify(
                    [this]
                    {
                        schedule();
                    }
                );
            }

            // Same accepted narrow race as mouse_snake_trail's TrailPump:
            // set_notify({}) does not join an in-flight notify, so one extra
            // drain can land after the fence; it finds an empty queue and
            // runs while this object is still alive (run() destroys the pump
            // only after Session::close() joins the reactor).
            [[nodiscard]]
            grab::Result<void>
            stop()
            {
                subscription_.set_notify( {} );
                session_->stop_observation();
                std::promise<void> fence;
                auto               reached = fence.get_future();
                auto               posted  = session_->post(
                    [this, &fence]
                    {
                        drain();
                        logger_->flush_pending();
                        fence.set_value();
                    }
                );
                if( !posted.has_value() )
                {
                    return posted;
                }
                reached.get();
                return {};
            }

        private:

            void
            schedule()
            {
                bool expected = false;
                if( !scheduled_.compare_exchange_strong( expected, true ) )
                {
                    return;
                }
                auto posted = session_->post(
                    [this]
                    {
                        drain();
                    }
                );
                if( !posted.has_value() )
                {
                    scheduled_.store( false );
                    logger_->remember_error( std::move( posted.error() ) );
                }
            }

            void
            drain()
            {
                scheduled_.store( false );
                while( auto item = subscription_.try_pop_item() )
                {
                    logger_->consume( *item );
                }
            }

            grab::Session*     session_;
            grab::Subscription subscription_;
            EventLogger*       logger_;
            std::atomic_bool   scheduled_{ false };
    };

    // ---- signals --------------------------------------------------------

    [[nodiscard]]
    bool
    block_shutdown_signals( sigset_t& signals ) noexcept    // NOLINT(misc-include-cleaner)
    {
        // NOLINTNEXTLINE(misc-include-cleaner): POSIX <signal.h>.
        if( ::sigemptyset( &signals ) != signalSuccess )
        {
            return false;
        }
        // NOLINTNEXTLINE(misc-include-cleaner): POSIX <signal.h>.
        if( ::sigaddset( &signals, SIGINT ) != signalSuccess ||
            ::sigaddset( &signals, SIGTERM ) != signalSuccess )
        {
            return false;
        }
        // NOLINTNEXTLINE(misc-include-cleaner): POSIX <signal.h>.
        return ::pthread_sigmask( SIG_BLOCK, &signals, nullptr ) == signalSuccess;
    }

    // One 300 ms poll tick; returns true when SIGINT/SIGTERM arrived.
    [[nodiscard]]
    bool
    shutdown_requested( const sigset_t& signals ) noexcept    // NOLINT(misc-include-cleaner)
    {
        const timespec timeout{ .tv_sec = 0, .tv_nsec = pollTickNanos };
        // NOLINTNEXTLINE(misc-include-cleaner): POSIX <signal.h>.
        const int received = ::sigtimedwait( &signals, nullptr, &timeout );
        return received == SIGINT || received == SIGTERM;
    }

    // ---- main flow ------------------------------------------------------

    [[nodiscard]]
    grab::Result<void>
    run()
    {
        sigset_t signals{};
        // Block BEFORE Session::open(): the session's reactor thread (and
        // every other later-spawned thread) must inherit the mask, or a
        // process-directed SIGINT could be delivered to an unblocked worker.
        if( !block_shutdown_signals( signals ) )
        {
            return std::unexpected( grab::Error{
                .code    = grab::ErrorCode::Internal,
                .message = "failed to block shutdown signals",
            } );
        }

        auto session = grab::Session::open();
        if( !session.has_value() )
        {
            return std::unexpected( std::move( session.error() ) );
        }

        EventLogger             logger;

        grab::SubscriptionScope scope;    // empty scope + wildcard filter = all kinds
        auto subscription = ( *session )->watch(
            std::move( scope ),
            grab::QueueOptions{
                .capacity = queueCapacity,
                .overflow = grab::QueueOverflowPolicy::NeverDrop,
            }
        );
        if( !subscription.has_value() )
        {
            return std::unexpected( std::move( subscription.error() ) );
        }

        LogPump pump{ **session, std::move( *subscription ), logger };
        pump.install();

        auto observing = ( *session )->start_observation();
        if( !observing.has_value() )
        {
            return std::unexpected( std::move( observing.error() ) );
        }

        std::cout << "event_logger: observing (Ctrl+C to stop)\n";
        std::cout.flush();

        while( !shutdown_requested( signals ) )
        {
            // Tick: Task 4 posts a coalescer flush here.
        }

        auto stopped = pump.stop();
        ( *session )->close();

        std::cout << "event_logger: " << logger.observed() << " events observed, "
                  << logger.printed() << " lines printed, " << logger.gaps()
                  << " gaps\n";
        std::cout.flush();

        if( !stopped.has_value() )
        {
            return stopped;
        }
        if( auto error = logger.error(); error.has_value() )
        {
            return std::unexpected( std::move( *error ) );
        }
        return {};
    }

}    // namespace

int
main()
{
    auto result = run();
    if( !result.has_value() )
    {
        std::cerr << "event_logger: " << result.error().message << '\n';
        return 1;
    }
    return 0;
}
```

Check `grab::Error` / `grab::ErrorCode::Internal` member spelling against `include/grab/result.hpp` before building; if `Internal` does not exist, pick the closest generic code that does (the CLI's `print_fatal` sites show which are in use).

- [ ] **Step 3: Build warning-clean**

```bash
cmake --build build -j$(nproc)
```

Expected: `event_logger` builds; clang-format/clang-tidy pass. Fix any tidy naming/format complaints now.

- [ ] **Step 4: Manual smoke under Xvfb**

```bash
DISPLAY=:97 ./build/examples/event_logger > /tmp/feed.txt & LOGGER=$!
sleep 1
command -v xdotool >/dev/null && DISPLAY=:97 xdotool key a click 1
sleep 1
kill -INT $LOGGER; wait $LOGGER; echo "exit=$?"
cat /tmp/feed.txt
```

Expected: exit=0; feed contains `input   event -> key ...` and `... button ... clicked` lines plus raw per-sample `pointer moved` lines (coalescing lands in Task 4), and the final summary line.

- [ ] **Step 5: Commit**

```bash
git add examples/event_logger.cpp examples/CMakeLists.txt
git commit -m "feat: event_logger example skeleton with full-vocabulary formatter"
```

---

### Task 4: MoveCoalescer — one pointer line per ~300 ms window

**Files:**
- Modify: `examples/event_logger.cpp`

**Interfaces:**
- Consumes: `EventLogger`, `feed_line`, `format_timestamp` from Task 3.
- Produces: `class MoveCoalescer` with `std::optional<std::string> feed( const grab::Event& )` and `std::optional<std::string> flush()`; `EventLogger::flush_pending()` becomes real; `run()`'s poll loop posts a tick.

- [ ] **Step 1: Add the coalescer**

Add above `EventLogger` (constants join the existing block):

```cpp
    constexpr double coalesceWindowSeconds = 0.3;

    // Collapses continuous MouseMove streams into one summary line per
    // window. Discrete kinds never pass through here.
    class MoveCoalescer
    {
        public:

            // Returns the finished summary line when the pending window
            // closes; the new sample opens the next window.
            [[nodiscard]]
            std::optional<std::string>
            feed( const grab::Event& event )
            {
                const auto* move = std::get_if<grab::MouseMove>( &event.payload );
                if( move == nullptr )
                {
                    return std::nullopt;
                }
                std::optional<std::string> line;
                if( pending_.has_value() &&
                    event.timestamp - pending_->first_ts >= coalesceWindowSeconds )
                {
                    line = render( *pending_ );
                    pending_.reset();
                }
                if( !pending_.has_value() )
                {
                    pending_ = Pending{ .first_ts = event.timestamp };
                }
                ++pending_->samples;
                if( move->position.has_value() )
                {
                    pending_->last_position = *move->position;
                }
                return line;
            }

            [[nodiscard]]
            std::optional<std::string>
            flush()
            {
                if( !pending_.has_value() )
                {
                    return std::nullopt;
                }
                auto line = render( *pending_ );
                pending_.reset();
                return line;
            }

        private:

            struct Pending
            {
                    double                          first_ts = 0.0;
                    std::size_t                     samples  = 0U;
                    std::optional<grab::SpacePoint> last_position;
            };

            [[nodiscard]]
            static std::string
            render( const Pending& pending )
            {
                std::string description;
                if( pending.last_position.has_value() )
                {
                    description =
                        "pointer moved to (" +
                        std::to_string(
                            static_cast<std::int64_t>( pending.last_position->x ) ) +
                        ", " +
                        std::to_string(
                            static_cast<std::int64_t>( pending.last_position->y ) ) +
                        ")";
                }
                else
                {
                    description = "pointer moved";
                }
                description += " [" + std::to_string( pending.samples ) + " samples]";
                return feed_line( pending.first_ts,
                                  grab::EventKind::MouseMove,
                                  description );
            }

            std::optional<Pending> pending_;
    };
```

- [ ] **Step 2: Route moves through it and keep ordering**

In `EventLogger`:
- add a member `MoveCoalescer coalescer_;`
- in `consume`, replace the unconditional `print( feed_line( ... ) )` for events with:

```cpp
                const auto& event = std::get<grab::Event>( item );
                ++observed_;
                if( event.kind == grab::EventKind::MouseMove )
                {
                    if( auto line = coalescer_.feed( event ); line.has_value() )
                    {
                        print( *line );
                    }
                    return;
                }
                // A discrete event flushes the pending pointer summary first
                // so the feed stays chronological.
                flush_pending();
                print( feed_line( event.timestamp, event.kind, describe( event ) ) );
```

- make `flush_pending()` real:

```cpp
            void
            flush_pending()
            {
                if( auto line = coalescer_.flush(); line.has_value() )
                {
                    print( *line );
                }
            }
```

Also flush before the gap line in the `QueueGapMarker` branch (insert `flush_pending();` before `print( "!! gap: ..." )`).

- [ ] **Step 3: Post the tick from the poll loop**

In `run()`, replace the empty tick comment:

```cpp
        while( !shutdown_requested( signals ) )
        {
            // Bounded staleness: a pointer that stops moving still gets its
            // final summary within one poll tick.
            auto ticked = ( *session )->post(
                [&logger]
                {
                    logger.flush_pending();
                }
            );
            if( !ticked.has_value() )
            {
                logger.remember_error( std::move( ticked.error() ) );
                break;
            }
        }
```

- [ ] **Step 4: Build + smoke**

```bash
cmake --build build -j$(nproc)
DISPLAY=:97 ./build/examples/event_logger > /tmp/feed.txt & LOGGER=$!
sleep 1
command -v xdotool >/dev/null && DISPLAY=:97 xdotool mousemove 100 100 mousemove 400 400 mousemove 700 500
sleep 1
kill -INT $LOGGER; wait $LOGGER
grep "pointer moved" /tmp/feed.txt
```

Expected: a small number of `pointer moved ... [N samples]` lines (N > 1 for at least one), not one line per sample.

- [ ] **Step 5: Commit**

```bash
git add examples/event_logger.cpp
git commit -m "feat: coalesce continuous pointer motion in event_logger"
```

---

### Task 5: WindowTracker on the session seam — os lines

**Files:**
- Modify: `examples/event_logger.cpp`

**Interfaces:**
- Consumes: `grab::drivers::desktop::x11::WindowTracker::start( const char*, grab::core::Reactor&, grab::EventBus&, std::chrono::milliseconds )` → `Result<WindowTracker>`; `WindowTracker::stop()`; `Session::reactor()`, `Session::bus()`.
- Produces: os-category lines (`WindowCreated/Closed/FocusChanged/TitleChanged`) in the feed.

- [ ] **Step 1: Start the tracker after observation is live**

Add `#include "drivers/desktop/x11/window_tracker.hpp"` to the example. In `run()`, right after the `start_observation()` success check:

```cpp
        // The owning Session composes input + AT-SPI + tree sources but no
        // WindowTracker; start one on the session's own reactor and bus
        // (the public composition seam) so os-category events join the feed.
        auto tracker = grab::drivers::desktop::x11::WindowTracker::start(
            nullptr,
            ( *session )->reactor(),
            ( *session )->bus()
        );
        if( !tracker.has_value() )
        {
            return std::unexpected( std::move( tracker.error() ) );
        }
```

In the shutdown path, stop producers before the pump (spec order):

```cpp
        tracker->stop();
        auto stopped = pump.stop();
```

- [ ] **Step 2: Build + smoke**

```bash
cmake --build build -j$(nproc)
DISPLAY=:97 ./build/examples/event_logger > /tmp/feed.txt & LOGGER=$!
sleep 1
DISPLAY=:97 xterm -e sleep 2 &
sleep 3
kill -INT $LOGGER; wait $LOGGER
grep "os      event" /tmp/feed.txt
```

Expected: `application "xterm" opened window ...` / `... focused` / `... closed ...` lines.

- [ ] **Step 3: Commit**

```bash
git add examples/event_logger.cpp
git commit -m "feat: window tracker feeds os events into event_logger"
```

---

### Task 6: JSONL recording, gap surfacing, summary

**Files:**
- Modify: `examples/event_logger.cpp`

**Interfaces:**
- Consumes: `grab::storage::JsonlSink::open( JsonlOptions{ .dir } )` → `Result<JsonlSink>`; `write( const Event& )` → `Result<void>`; `flush()` → `Result<void>`; `close()` (void, swallows errors — flush explicitly first). Include `"storage/jsonl_sink.hpp"`.
- Produces: `EventLogger::attach_sink( grab::storage::JsonlSink* )`; per-run recording under the chosen dir; exit code 1 on remembered sink error.

- [ ] **Step 1: Record every surviving event**

In `EventLogger`: add members

```cpp
            grab::storage::JsonlSink* sink_        = nullptr;
            bool                      sink_failed_ = false;
```

add

```cpp
            void
            attach_sink( grab::storage::JsonlSink* sink ) noexcept
            {
                sink_ = sink;
            }

            // Recording must never kill the live feed: first failure warns
            // once on stderr, stops recording, and is reported at exit.
            void
            record( const grab::Event& event )
            {
                if( sink_ == nullptr || sink_failed_ )
                {
                    return;
                }
                auto written = sink_->write( event );
                if( !written.has_value() )
                {
                    sink_failed_ = true;
                    std::cerr << "event_logger: recording stopped: "
                              << written.error().message << '\n';
                    remember_error( std::move( written.error() ) );
                }
            }
```

and call `record( event );` in `consume` immediately after `++observed_;` — **before** the coalescer branch, so the JSONL keeps every raw move sample while the terminal coalesces.

- [ ] **Step 2: Open the sink in `run()` and finish it in shutdown**

Argument parsing at the top of `run()` (signature becomes `run( std::span<char*> args )`; `main` passes `std::span{ argv, static_cast<std::size_t>( argc ) }.subspan( 1 )`; add `<span>` and `<filesystem>` includes):

```cpp
        std::filesystem::path recording_dir{ "event-log" };
        if( !args.empty() && std::string_view{ args.front() } != "--socket" )
        {
            recording_dir = args.front();
        }
```

(The `--socket` flag itself is consumed in Task 7; until then unknown flags may simply be ignored by this positional check.)

After the session opens:

```cpp
        auto sink = grab::storage::JsonlSink::open(
            grab::storage::JsonlOptions{ .dir = recording_dir }
        );
        if( !sink.has_value() )
        {
            return std::unexpected( std::move( sink.error() ) );
        }
        logger.attach_sink( &*sink );
```

In shutdown, after `( *session )->close();` (the fence has run; the sink is no longer touched by the reactor):

```cpp
        auto flushed = sink->flush();
        sink->close();
        if( !flushed.has_value() && !logger.error().has_value() )
        {
            logger.remember_error( std::move( flushed.error() ) );
        }
```

Extend the summary print with `", recording: " << recording_dir.string()`.

- [ ] **Step 3: Build + smoke**

```bash
cmake --build build -j$(nproc)
cd /tmp && rm -rf ev && mkdir ev && DISPLAY=:97 <worktree>/build/examples/event_logger ev > feed.txt & LOGGER=$!
sleep 1
command -v xdotool >/dev/null && DISPLAY=:97 xdotool mousemove 10 10 mousemove 300 300 key a
sleep 1
kill -INT $LOGGER; wait $LOGGER
ls ev/*.jsonl && wc -l ev/*.jsonl && grep -c "pointer moved" feed.txt
```

Expected: a dated `.jsonl` with plausible epoch timestamps whose line count exceeds the number of printed pointer lines (raw samples recorded, terminal coalesced).

- [ ] **Step 4: Commit**

```bash
git add examples/event_logger.cpp
git commit -m "feat: JSONL recording and gap surfacing in event_logger"
```

---

### Task 7: Browser socket — BrowserBridge behind a unix listener + tab_title fallback

**Files:**
- Modify: `examples/event_logger.cpp`

**Interfaces:**
- Consumes: `grab::drivers::semantic::webextension::BrowserBridge::start( int fd, grab::core::Reactor&, grab::EventBus& )` → `Result<BrowserBridge>` (move-only; `stop()`; does **not** take fd ownership — verified, the bridge never closes it); `Reactor::add_fd( int, std::uint32_t, std::function<void(std::uint32_t)> )` → `std::uint64_t`, `Reactor::remove_fd( std::uint64_t )`; `nlohmann::json` (public via `grab_core`).
- Produces: `class BrowserSocket` with `static grab::Result<BrowserSocket> open( std::string path, grab::Session& )` and `void stop( grab::Session& )`; browser lines in the feed; `--socket <path>` flag.

- [ ] **Step 1: tab_title fallback in the formatter**

The real extension protocol sends `tab_title` while the bridge fills `IntegrationEvent::title` only from `title`. Add near `describe` (include `<nlohmann/json.hpp>`):

```cpp
    constexpr std::string_view tabTitleKey = "tab_title";

    [[nodiscard]]
    std::string
    integration_title( const grab::IntegrationEvent& event )
    {
        if( !event.title.empty() )
        {
            return event.title;
        }
        const auto parsed =
            nlohmann::json::parse( event.json, nullptr, /*allow_exceptions=*/false );
        if( parsed.is_object() )
        {
            if( const auto entry = parsed.find( tabTitleKey );
                entry != parsed.end() && entry->is_string() )
            {
                return entry->get<std::string>();
            }
        }
        return {};
    }
```

and in `describe`'s `AppTabChanged` branch use `integration_title( *app )` instead of `app->title`.

- [ ] **Step 2: The listener**

Add includes `<sys/socket.h>`, `<sys/un.h>`, `<unistd.h>`, `<cerrno>`, `<memory>`, `<vector>`, `<cstring>`, `<cstdlib>`, and:

```cpp
    constexpr int           listenBacklog   = 4;
    constexpr std::uint32_t epollInEvents   = EPOLLIN;    // include <sys/epoll.h>
    constexpr int           invalidFd       = -1;

    [[nodiscard]]
    std::string
    default_socket_path()
    {
        // NOLINTNEXTLINE(concurrency-mt-unsafe): read once before threads spawn.
        const char* runtime_dir = std::getenv( "XDG_RUNTIME_DIR" );
        const std::string base =
            runtime_dir != nullptr ? std::string{ runtime_dir } : std::string{ "/tmp" };
        return base + "/grab-event-logger.sock";
    }

    // Accepts native-messaging connections and hands each to a BrowserBridge
    // on the session's reactor/bus. Connection state is touched only on the
    // reactor thread; stop() serializes through a posted fence.
    class BrowserSocket
    {
        public:

            [[nodiscard]]
            static grab::Result<BrowserSocket>
            open( std::string    path,
                  grab::Session& session )
            {
                sockaddr_un address{};
                if( path.size() >= sizeof( address.sun_path ) )
                {
                    return std::unexpected( grab::Error{
                        .code    = grab::ErrorCode::InvalidArgument,
                        .message = "socket path too long: " + path,
                    } );
                }
                const int fd = ::socket( AF_UNIX,
                                         SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                                         0 );
                if( fd == invalidFd )
                {
                    return std::unexpected( grab::Error{
                        .code    = grab::ErrorCode::Internal,
                        .message = "socket() failed: " +
                                   std::string{ std::strerror( errno ) },
                    } );
                }
                address.sun_family = AF_UNIX;
                std::strncpy( address.sun_path,
                              path.c_str(),
                              sizeof( address.sun_path ) - 1U );
                ::unlink( path.c_str() );    // stale socket from a prior run
                if( ::bind( fd,
                            reinterpret_cast<const sockaddr*>( &address ),
                            sizeof( address ) ) != 0 ||
                    ::listen( fd, listenBacklog ) != 0 )
                {
                    const int saved = errno;
                    ::close( fd );
                    return std::unexpected( grab::Error{
                        .code    = grab::ErrorCode::Internal,
                        .message = "bind/listen failed on " + path + ": " +
                                   std::string{ std::strerror( saved ) },
                    } );
                }

                BrowserSocket socket;
                socket.path_      = std::move( path );
                socket.listen_fd_ = fd;
                socket.state_     = std::make_shared<State>();
                auto* const state = socket.state_.get();
                auto&       reactor = session.reactor();
                auto&       bus     = session.bus();
                socket.token_ = reactor.add_fd(
                    fd,
                    epollInEvents,
                    [fd, state, &reactor, &bus]( std::uint32_t )
                    {
                        accept_pending( fd, *state, reactor, bus );
                    }
                );
                return socket;
            }

            void
            stop( grab::Session& session )
            {
                if( listen_fd_ == invalidFd )
                {
                    return;
                }
                session.reactor().remove_fd( token_ );
                // Serialize with any in-flight accept callback.
                std::promise<void> fence;
                auto               reached = fence.get_future();
                auto               posted  = session.post(
                    [this, &fence]
                    {
                        for( auto& connection : state_->connections )
                        {
                            connection.bridge.stop();
                            ::close( connection.fd );
                        }
                        state_->connections.clear();
                        fence.set_value();
                    }
                );
                if( posted.has_value() )
                {
                    reached.get();
                }
                ::close( listen_fd_ );
                ::unlink( path_.c_str() );
                listen_fd_ = invalidFd;
            }

        private:

            struct Connection
            {
                    int                                                fd;
                    grab::drivers::semantic::webextension::BrowserBridge bridge;
            };

            struct State
            {
                    std::vector<Connection> connections;    // reactor thread only
            };

            static void
            accept_pending( int                  listen_fd,
                            State&               state,
                            grab::core::Reactor& reactor,
                            grab::EventBus&      bus )
            {
                for( ;; )
                {
                    const int fd = ::accept4( listen_fd,
                                              nullptr,
                                              nullptr,
                                              SOCK_NONBLOCK | SOCK_CLOEXEC );
                    if( fd == invalidFd )
                    {
                        return;    // EAGAIN or transient error: wait for next EPOLLIN
                    }
                    auto bridge =
                        grab::drivers::semantic::webextension::BrowserBridge::start(
                            fd,
                            reactor,
                            bus );
                    if( !bridge.has_value() )
                    {
                        ::close( fd );
                        continue;
                    }
                    state.connections.push_back(
                        Connection{ .fd = fd, .bridge = std::move( *bridge ) } );
                }
            }

            std::string            path_;
            int                    listen_fd_ = invalidFd;
            std::uint64_t          token_     = 0U;
            std::shared_ptr<State> state_;
    };
```

Add `#include "drivers/semantic/webextension/browser_bridge.hpp"` and `<sys/epoll.h>`. Check `grab::ErrorCode::InvalidArgument`/`Internal` spellings against `include/grab/result.hpp` and substitute existing codes if needed. `BrowserSocket` needs a default constructor plus move support consistent with holding a raw fd — default-member-initialize everything, delete copy, and implement move by `std::exchange` on `listen_fd_`/`token_` (mirror the `UniqueFd` pattern in `tests/drivers/semantic/webextension/test_browser_bridge.cpp`).

- [ ] **Step 3: Wire flag + lifecycle in `run()`**

Extend argument parsing:

```cpp
        std::string socket_path = default_socket_path();
        for( std::size_t index = 0U; index < args.size(); ++index )
        {
            const std::string_view arg{ args[ index ] };
            if( arg == "--socket" && index + 1U < args.size() )
            {
                socket_path = args[ index + 1U ];
                ++index;
            }
            else if( index == 0U )
            {
                recording_dir = arg;
            }
        }
```

(replacing Task 6's simpler positional parse). After the tracker starts:

```cpp
        auto browser_socket = BrowserSocket::open( socket_path, **session );
        if( !browser_socket.has_value() )
        {
            return std::unexpected( std::move( browser_socket.error() ) );
        }
        std::cout << "event_logger: browser socket at " << socket_path << '\n';
        std::cout.flush();
```

Shutdown order (spec): `browser_socket->stop( **session );` then `tracker->stop();` then `pump.stop()`.

- [ ] **Step 4: Wiring doc in the header comment**

Extend the file's top comment:

```cpp
// Browser wiring: register a native-messaging host in your browser whose
// executable forwards stdio to this socket, e.g.
//   #!/bin/sh
//   exec socat STDIO UNIX-CONNECT:"$XDG_RUNTIME_DIR/grab-event-logger.sock"
// Manifest (Chrome: ~/.config/google-chrome/NativeMessagingHosts/<name>.json,
// Firefox: ~/.mozilla/native-messaging-hosts/<name>.json) points "path" at
// that script; the grab webextension then streams tab events here.
```

- [ ] **Step 5: Build + canned-frame smoke**

```bash
cmake --build build -j$(nproc)
SOCK=/tmp/ev-smoke.sock
DISPLAY=:97 ./build/examples/event_logger /tmp/ev --socket "$SOCK" > /tmp/feed.txt & LOGGER=$!
sleep 1
python3 - "$SOCK" <<'EOF'
import socket, struct, sys
frame = b'{"type":"browser.tab_switched","tab_title":"SmokeTab","app":"chrome","pid":"1"}'
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1]); s.sendall(struct.pack('<I', len(frame)) + frame); s.close()
EOF
sleep 1
kill -INT $LOGGER; wait $LOGGER
grep 'browser event -> tab "SmokeTab" focused' /tmp/feed.txt
```

Expected: the grep matches (native-messaging framing is 4-byte little-endian length + JSON, exactly what the bridge tests feed it).

- [ ] **Step 6: Commit**

```bash
git add examples/event_logger.cpp
git commit -m "feat: browser bridge socket and tab title fallback in event_logger"
```

---

### Task 8: Smoke script + final verification

**Files:**
- Create: `examples/event_logger_smoke.sh` (executable)

**Interfaces:**
- Consumes: the finished `event_logger` binary and everything above.
- Produces: one command that exercises input/os/browser/JSONL end-to-end.

- [ ] **Step 1: Write the script**

Create `examples/event_logger_smoke.sh` (`chmod +x`):

```bash
#!/usr/bin/env bash
# Smoke for examples/event_logger: Xvfb + xterm (os events), xdotool
# (input events, skipped if absent), a canned native-messaging frame
# (browser events), and JSONL assertions. Usage: event_logger_smoke.sh [build-dir]
set -euo pipefail

BUILD_DIR="${1:-build}"
DISPLAY_NUM=":97"
WORK="$(mktemp -d)"
SOCK="$WORK/browser.sock"
FEED="$WORK/feed.txt"
cleanup() {
    kill "${LOGGER_PID:-0}" "${XVFB_PID:-0}" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

Xvfb "$DISPLAY_NUM" -noreset -screen 0 1280x800x24 &
XVFB_PID=$!
sleep 1

DISPLAY="$DISPLAY_NUM" "$BUILD_DIR/examples/event_logger" "$WORK/events" --socket "$SOCK" > "$FEED" &
LOGGER_PID=$!
sleep 1

DISPLAY="$DISPLAY_NUM" xterm -e sleep 2 &
sleep 1
if command -v xdotool >/dev/null; then
    DISPLAY="$DISPLAY_NUM" xdotool key a click 1 mousemove 100 100 mousemove 500 400
else
    echo "smoke: xdotool absent, skipping input assertions"
fi

python3 - "$SOCK" <<'EOF'
import socket, struct, sys
frame = b'{"type":"browser.tab_switched","tab_title":"SmokeTab","app":"chrome","pid":"1"}'
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1]); s.sendall(struct.pack('<I', len(frame)) + frame); s.close()
EOF
sleep 2

kill -INT "$LOGGER_PID"
wait "$LOGGER_PID"

grep -q 'os      event' "$FEED"
grep -q 'browser event -> tab "SmokeTab" focused' "$FEED"
if command -v xdotool >/dev/null; then
    grep -q 'input   event -> key' "$FEED"
    grep -q 'pointer moved' "$FEED"
fi
ls "$WORK"/events/*.jsonl >/dev/null
echo "event_logger smoke OK"
```

- [ ] **Step 2: Run everything**

```bash
cmake --build build -j$(nproc)
DISPLAY=:97 ctest --test-dir build --output-on-failure
./examples/event_logger_smoke.sh build
```

Expected: full test suite green; script prints `event_logger smoke OK`. If any grep fails, read `$FEED` before touching code — the feed itself is the diagnostic.

- [ ] **Step 3: Real-display sanity run (manual, brief)**

On the real desktop: `./build/examples/event_logger`, move the mouse, switch windows, Ctrl+C. Confirm readable aligned output and a sane summary. (Browser lines need the manifest wiring from Task 7's doc comment — optional here.)

- [ ] **Step 4: Commit**

```bash
git add examples/event_logger_smoke.sh
git commit -m "test: end-to-end smoke script for event_logger"
```

---

## Plan Self-Review (done at authoring)

- **Spec coverage:** timestamps pre-fix → Tasks 1–2; formatter/labels/fallbacks → Task 3 (+ tab_title in 7); coalescer + tick → Task 4; WindowTracker seam → Task 5; JSONL best-effort + gaps + summary + sink-error path → Task 6; browser socket + wiring doc → Task 7; smoke (input/os/browser/JSONL) → Task 8. Known-limitations prose (swallowed X11 error, silent observer death) is documentation in the example header — covered by Task 3's comment plus spec.
- **Placeholders:** none; every code step is concrete. Two deliberate verify-then-adjust notes (ErrorCode spellings, other tests asserting old timestamps) name the exact file to check.
- **Type consistency:** `EventLogger::consume/flush_pending/record/attach_sink`, `MoveCoalescer::feed/flush`, `LogPump::install/stop`, `BrowserSocket::open/stop` used consistently across tasks; `feed_line( double, EventKind, const std::string& )` shared by formatter and coalescer.
