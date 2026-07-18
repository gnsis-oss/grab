# mouse_snake_trail Example Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `examples/mouse_snake_trail` — the first buildable C++ example: it sweeps the pointer top-left → bottom-right in a boustrophedon (snake) pattern while an observation-driven, fading comet-tail trail follows it on the overlay.

**Architecture:** One process, two roles. The main thread drives `grab::Input::move` along precomputed snake waypoints. The session reactor thread observes the resulting `MouseMove` events (`Session::watch` + `start_observation`, notify→post→drain) and draws one fading `Path{MoveTo,LineTo}` segment per move via the public `grab::Overlay` facade. Spec: `docs/superpowers/specs/2026-07-18-mouse-snake-trail-example-design.md`.

**Tech Stack:** C++23, public grab API only (`Session`, `Overlay`, `Input`, `Screen`, `watch`), CMake, Xvfb + the `grab` CLI + python3/PIL for smoke verification.

## Global Constraints

- Work in the worktree `/home/gn/ws/grab_workspace/grab/.worktrees/integrate` (branch `feat/grab-port`). All paths below are relative to it.
- Public API only in the example: include nothing from `src/` — only `include/grab/*.hpp` headers.
- Toolchain gates run on every build: warnings-as-errors (`-Wall -Wextra -Wconversion -Wshadow -Wpedantic -Werror`), clang-format, clang-tidy, ASan/UBSan. A clean build IS the static test.
- Naming: CamelCase enum values, lowerCamelCase `constexpr` constants (no `k` prefix), snake_case functions, trailing `_` members. Named constants for every numeric value.
- Commit as the configured git user only; after each commit verify with `git log -1 --format='%an <%ae> / %cn <%ce>'` that it shows `GNERSIS <gornersisyan4@gmail.com>` and nothing else. No co-author trailers.
- Build commands: `cmake --build build -j$(nproc)` (the `build/` dir is already configured with the default clang+ninja preset; editing CMakeLists re-triggers configure automatically).
- Xvfb displays `:87`–`:89` and `:95` are reserved (test fixtures / demo script). Use `:96` for this plan's smoke runs.
- Origin expectations: the example's `Input` connection is separate from the session seat, so its XTest moves are classified `EventOrigin::InjectedOther` → drawn in the injected (blue) color. Physical moves draw red. The renderer draws ALL positioned moves (unlike the kernel `TrailAnimator`, which skips `InjectedOther` — that skip would erase our own snake).

---

### Task 1: Build wiring + skeleton example

**Files:**
- Create: `examples/CMakeLists.txt`
- Create: `examples/mouse_snake_trail.cpp`
- Modify: `CMakeLists.txt` (root — next to the `if(BUILD_TESTING)` block near line 302)

**Interfaces:**
- Consumes: existing library targets `grab_core`, `grab_kernel`, `grab_session`, `grab_platform_x11`, `grab_input`, `grab_input_x11`, `grab_screen`, `grab_image`, `grab_codec` (same set the CLI links, minus `grab_notify`/`grab_service`/`grab_client`).
- Produces: executable target `mouse_snake_trail` at `build/mouse_snake_trail`; a `run()` function returning `grab::Result<void>` that later tasks extend.

- [ ] **Step 1: Write the skeleton example**

Create `examples/mouse_snake_trail.cpp`:

```cpp
#include "grab/result.hpp"
#include "grab/session.hpp"

#include <iostream>
#include <utility>

namespace
{

    [[nodiscard]]
    grab::Result<void>
    run()
    {
        auto session = grab::Session::open();
        if( !session.has_value() )
        {
            return std::unexpected( std::move( session.error() ) );
        }
        ( *session )->close();
        return {};
    }

}    // namespace

int
main()
{
    auto result = run();
    if( !result.has_value() )
    {
        std::cerr << "mouse_snake_trail: " << result.error().message << '\n';
        return 1;
    }
    return 0;
}
```

- [ ] **Step 2: Write the examples CMakeLists**

Create `examples/CMakeLists.txt`:

```cmake
add_executable(mouse_snake_trail mouse_snake_trail.cpp)
target_link_libraries(mouse_snake_trail PRIVATE
    grab_core
    grab_kernel
    grab_session
    grab_platform_x11
    grab_input
    grab_input_x11
    grab_screen
    grab_image
    grab_codec)
target_compile_features(mouse_snake_trail PRIVATE cxx_std_23)
```

If the link fails with undefined symbols, add the missing target from the CLI's link list (`CMakeLists.txt` ~line 297) rather than inventing new flags; if it links clean, optionally try removing targets one at a time to trim (not required).

- [ ] **Step 3: Wire examples into the root build**

In the root `CMakeLists.txt`, directly BEFORE the `if(BUILD_TESTING)` block, add:

```cmake
option(GRAB_BUILD_EXAMPLES "Build example programs" ON)
if(GRAB_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
```

- [ ] **Step 4: Build (this is the test — format, tidy, warnings all gate)**

Run: `cmake --build build -j$(nproc)`
Expected: configure re-runs, `mouse_snake_trail` compiles and links; zero warnings; `build/mouse_snake_trail` exists.

- [ ] **Step 5: Smoke-run under Xvfb**

```bash
Xvfb :96 -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1
DISPLAY=:96 ./build/mouse_snake_trail; echo "exit=$?"
kill $XVFB_PID
```

Expected: `exit=0`, no output on stderr.

- [ ] **Step 6: Commit**

```bash
git add examples/CMakeLists.txt examples/mouse_snake_trail.cpp CMakeLists.txt
git commit -m "build: wire examples into the build, add mouse_snake_trail skeleton"
git log -1 --format='%an <%ae> / %cn <%ce>'
```

Expected: authorship shows `GNERSIS <gornersisyan4@gmail.com>` twice.

---

### Task 2: Snake waypoints (compile-time tested) + pointer sweep

**Files:**
- Modify: `examples/mouse_snake_trail.cpp`

**Interfaces:**
- Consumes: `grab::Input::open()` / `Input::move(std::int16_t, std::int16_t)` (absolute root coordinates); `grab::Screen::open()` / `Screen::display()` → `grab::Image` with public `std::uint32_t width, height` members.
- Produces: `struct PixelPoint { std::int32_t x, y; }`; `constexpr std::array<PixelPoint, snakePassCount * 2U> snake_waypoints(std::int32_t width, std::int32_t height)`; `grab::Result<void> move_line(grab::Input&, const PixelPoint&, const PixelPoint&)`. Task 3 reuses `run()`'s session/screen/input scaffolding as written here.

- [ ] **Step 1: Write the compile-time contract FIRST, against a stub**

In `examples/mouse_snake_trail.cpp`, extend the includes to:

```cpp
#include "grab/image.hpp"
#include "grab/input.hpp"
#include "grab/result.hpp"
#include "grab/screen.hpp"
#include "grab/session.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <utility>
```

At the top of the anonymous namespace, add the constants, `PixelPoint`, a deliberately WRONG stub, and the compile-time tests:

```cpp
    // Sweep geometry. The pass count is odd so the final pass travels
    // left->right and the sweep ends at the inset bottom-right corner.
    constexpr std::int32_t              screenMarginPx = 40;
    constexpr std::size_t               snakePassCount = 9U;
    constexpr std::int32_t              sweepStepPx    = 12;
    constexpr std::chrono::milliseconds sweepStepInterval{ 6 };

    struct PixelPoint
    {
            std::int32_t x = 0;
            std::int32_t y = 0;

            friend constexpr bool
            operator==( const PixelPoint&,
                        const PixelPoint& ) = default;
    };

    [[nodiscard]]
    constexpr std::array<PixelPoint, snakePassCount * 2U>
    snake_waypoints( std::int32_t width,
                     std::int32_t height )
    {
        static_cast<void>( width );
        static_cast<void>( height );
        return {};    // stub: fails the static_asserts below on purpose
    }

    // Compile-time contract on a reference display: the sweep starts at the
    // inset top-left corner, ends at the inset bottom-right corner, and
    // alternates direction (pass 0 rightward, pass 1 leftward).
    constexpr std::int32_t referenceWidth  = 1'920;
    constexpr std::int32_t referenceHeight = 1'080;
    constexpr auto         referenceWaypoints =
        snake_waypoints( referenceWidth, referenceHeight );
    static_assert( snakePassCount % 2U == 1U,
                   "odd pass count so the final pass ends bottom-right" );
    static_assert( referenceWaypoints.front() ==
                   PixelPoint{ screenMarginPx, screenMarginPx } );
    static_assert( referenceWaypoints.back() ==
                   PixelPoint{ referenceWidth - screenMarginPx,
                               referenceHeight - screenMarginPx } );
    static_assert( referenceWaypoints[1U].x == referenceWidth - screenMarginPx );
    static_assert( referenceWaypoints[2U].x == referenceWidth - screenMarginPx );
    static_assert( referenceWaypoints[3U].x == screenMarginPx );
```

- [ ] **Step 2: Build to verify the contract fails**

Run: `cmake --build build -j$(nproc) 2>&1 | grep -m1 static_assert`
Expected: FAIL — a `static_assert` diagnostic on `referenceWaypoints.front()`.

- [ ] **Step 3: Implement snake_waypoints for real**

Replace the stub body of `snake_waypoints` with:

```cpp
    [[nodiscard]]
    constexpr std::array<PixelPoint, snakePassCount * 2U>
    snake_waypoints( std::int32_t width,
                     std::int32_t height )
    {
        std::array<PixelPoint, snakePassCount * 2U> points{};
        const std::int32_t left   = screenMarginPx;
        const std::int32_t right  = width - screenMarginPx;
        const std::int32_t top    = screenMarginPx;
        const std::int32_t bottom = height - screenMarginPx;
        const std::int32_t spanY  = bottom - top;
        const auto lastPass = static_cast<std::int32_t>( snakePassCount ) - 1;
        for( std::size_t pass = 0U; pass < snakePassCount; ++pass )
        {
            const std::int32_t row =
                top +
                ( static_cast<std::int32_t>( pass ) * spanY ) / lastPass;
            const bool rightward       = pass % 2U == 0U;
            points[pass * 2U]          = PixelPoint{ rightward ? left : right, row };
            points[( pass * 2U ) + 1U] = PixelPoint{ rightward ? right : left, row };
        }
        return points;
    }
```

Consecutive passes share an x coordinate (a pass ends where the next one starts, one row down), so walking the flat waypoint list in order produces sweep, drop, sweep, drop, …

- [ ] **Step 4: Add the sweep driver and extend run()**

Below `snake_waypoints`, add:

```cpp
    [[nodiscard]]
    grab::Result<void>
    move_line( grab::Input&      input,
               const PixelPoint& from,
               const PixelPoint& to )
    {
        const std::int32_t deltaX = to.x - from.x;
        const std::int32_t deltaY = to.y - from.y;
        const std::int32_t distance =
            std::max( std::abs( deltaX ), std::abs( deltaY ) );
        const std::int32_t steps = std::max( distance / sweepStepPx, 1 );
        for( std::int32_t step = 1; step <= steps; ++step )
        {
            const std::int32_t x = from.x + ( ( deltaX * step ) / steps );
            const std::int32_t y = from.y + ( ( deltaY * step ) / steps );
            auto moved = input.move( static_cast<std::int16_t>( x ),
                                     static_cast<std::int16_t>( y ) );
            if( !moved.has_value() )
            {
                return moved;
            }
            std::this_thread::sleep_for( sweepStepInterval );
        }
        return {};
    }
```

Replace the whole `run()` function with:

```cpp
    [[nodiscard]]
    grab::Result<void>
    run()
    {
        auto session = grab::Session::open();
        if( !session.has_value() )
        {
            return std::unexpected( std::move( session.error() ) );
        }

        auto screen = grab::Screen::open();
        if( !screen.has_value() )
        {
            return std::unexpected( std::move( screen.error() ) );
        }
        auto frame = screen->display();
        if( !frame.has_value() )
        {
            return std::unexpected( std::move( frame.error() ) );
        }
        const auto width  = static_cast<std::int32_t>( frame->width );
        const auto height = static_cast<std::int32_t>( frame->height );

        auto input = grab::Input::open();
        if( !input.has_value() )
        {
            return std::unexpected( std::move( input.error() ) );
        }

        const auto waypoints = snake_waypoints( width, height );
        auto       placed =
            input->move( static_cast<std::int16_t>( waypoints.front().x ),
                         static_cast<std::int16_t>( waypoints.front().y ) );
        if( !placed.has_value() )
        {
            return placed;
        }
        for( std::size_t index = 1U; index < waypoints.size(); ++index )
        {
            auto swept =
                move_line( *input, waypoints[index - 1U], waypoints[index] );
            if( !swept.has_value() )
            {
                return swept;
            }
        }

        ( *session )->close();
        return {};
    }
```

- [ ] **Step 5: Build to verify the contract passes**

Run: `cmake --build build -j$(nproc)`
Expected: PASS — clean build, no warnings, no static_assert failures.

- [ ] **Step 6: Runtime smoke — pointer ends at the inset bottom-right**

```bash
Xvfb :96 -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1
DISPLAY=:96 ./build/mouse_snake_trail; echo "exit=$?"
DISPLAY=:96 xdotool getmouselocation
kill $XVFB_PID
```

Expected: `exit=0` and `x:1240 y:760 ...` (1280−40, 800−40). Takes ~6 s (nine 1200 px passes plus drops at ~2000 px/s).

- [ ] **Step 7: Commit**

```bash
git add examples/mouse_snake_trail.cpp
git commit -m "feat: snake sweep driver for mouse_snake_trail example"
git log -1 --format='%an <%ae> / %cn <%ce>'
```

Expected: authorship shows `GNERSIS <gornersisyan4@gmail.com>` twice.

---

### Task 3: Observation-driven fading trail

**Files:**
- Modify: `examples/mouse_snake_trail.cpp`

**Interfaces:**
- Consumes: `Session::overlay() -> Result<Overlay*>`, `Overlay::add(overlay::Shape) -> Result<overlay::ShapeId>`, `Overlay::flush() -> Result<void>`, `Overlay::space() -> Result<CoordinateSpaceId>`; `Session::watch(SubscriptionScope) -> Result<Subscription>`, `Subscription::set_notify(std::function<void()>)`, `Subscription::try_pop_item() -> std::optional<SubscriptionEvent>` where `SubscriptionEvent = std::variant<Event, QueueGapMarker>`; `Session::post(std::function<void()>)`, `start_observation()`, `stop_observation()`; payload struct `grab::MouseMove{ axis, delta, std::optional<SpacePoint> position }`; `grab::EventOrigin`.
- Produces: the finished example. Nothing downstream consumes it.

- [ ] **Step 1: Add the trail includes and style constants**

Extend the include block to (full final list):

```cpp
#include "grab/event.hpp"
#include "grab/image.hpp"
#include "grab/input.hpp"
#include "grab/origin.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/screen.hpp"
#include "grab/session.hpp"
#include "grab/space.hpp"
#include "grab/watch.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <variant>
#include <vector>
```

After the sweep constants, add the trail style (the built-in trail palette — injected blue, physical red):

```cpp
    constexpr std::uint8_t channelMax = std::numeric_limits<std::uint8_t>::max();
    constexpr grab::overlay::Color injectedTrailColor{
        .r = 0U, .g = 0U, .b = channelMax, .a = channelMax };
    constexpr grab::overlay::Color physicalTrailColor{
        .r = channelMax, .g = 0U, .b = 0U, .a = channelMax };
    constexpr std::chrono::milliseconds trailFade{ 1'200 };
    constexpr float                     trailWidthPx        = 3.0F;
    constexpr std::size_t               segmentCommandCount = 2U;
    // Hold after the sweep so the last segments fade out on screen.
    constexpr std::chrono::milliseconds endHold{ 1'500 };
```

- [ ] **Step 2: Add TrailRenderer**

After `move_line`, add. Note the deliberate divergence from the kernel `TrailAnimator`: it draws ALL positioned moves regardless of origin (our own injections arrive as `InjectedOther` because the example's `Input` connection is not the session seat — the kernel animator would skip them and no snake would appear). It breaks the path on origin or coordinate-space changes instead of mixing them into one segment.

```cpp
    class TrailRenderer
    {
        public:

            explicit TrailRenderer( grab::Overlay& overlay ) :
                overlay_{ &overlay }
            {
            }

            // Runs on the session reactor thread only.
            void
            consume( const grab::SubscriptionEvent& item )
            {
                if( std::holds_alternative<grab::QueueGapMarker>( item ) )
                {
                    previous_.reset();
                    return;
                }
                const auto& event = std::get<grab::Event>( item );
                if( event.kind != grab::EventKind::MouseMove )
                {
                    return;
                }
                const auto* const motion =
                    std::get_if<grab::MouseMove>( &event.payload );
                if( motion == nullptr || !motion->position.has_value() )
                {
                    previous_.reset();
                    return;
                }
                const grab::SpacePoint current = *motion->position;
                if( !previous_.has_value() ||
                    previous_->position.space !=
                    current.space ||
                    previous_->origin != event.origin )
                {
                    previous_ = Sample{ current, event.origin };
                    return;
                }
                auto added = overlay_->add(
                    trail_segment( previous_->position, current, event.origin )
                );
                if( !added.has_value() )
                {
                    remember_error( std::move( added.error() ) );
                }
                previous_ = Sample{ current, event.origin };
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

            struct Sample
            {
                    grab::SpacePoint  position{};
                    grab::EventOrigin origin{ grab::EventOrigin::Unknown };
            };

            [[nodiscard]]
            static grab::overlay::Shape
            trail_segment( const grab::SpacePoint& from,
                           const grab::SpacePoint& to,
                           grab::EventOrigin       origin )
            {
                std::vector<grab::overlay::PathCommand> commands;
                commands.reserve( segmentCommandCount );
                commands.emplace_back( grab::overlay::MoveTo{ .point = from } );
                commands.emplace_back( grab::overlay::LineTo{ .point = to } );
                const grab::overlay::Color color =
                    origin == grab::EventOrigin::Physical ? physicalTrailColor
                                                          : injectedTrailColor;
                return grab::overlay::Shape{
                    .geometry =
                        grab::overlay::Path{
                                            .commands = std::move( commands ),
                                            .closed   = false,
                                            },
                    .stroke =
                        grab::overlay::StrokeStyle{
                                            .color    = color,
                                            .width_px = trailWidthPx,
                                            },
                    .fill     = std::nullopt,
                    .lifetime = grab::overlay::Fade{ .duration = trailFade },
                    .band     = grab::overlay::Band::Trail,
                };
            }

            grab::Overlay*             overlay_;
            std::optional<Sample>      previous_;
            mutable std::mutex         error_mutex_;
            std::optional<grab::Error> error_;
    };
```

- [ ] **Step 3: Add TrailPump (notify → post → drain, with a shutdown fence)**

After `TrailRenderer`, add:

```cpp
    class TrailPump
    {
        public:

            TrailPump( grab::Session&     session,
                       grab::Subscription subscription,
                       TrailRenderer&     renderer ) :
                session_{ &session },
                subscription_{ std::move( subscription ) },
                renderer_{ &renderer }
            {
            }

            void
            install()
            {
                subscription_.set_notify( [this] { schedule(); } );
            }

            // Stops event flow and waits until every already-queued drain has
            // executed, so no reactor job touches this object afterwards.
            [[nodiscard]]
            grab::Result<void>
            stop()
            {
                subscription_.set_notify( {} );
                session_->stop_observation();
                return reactor_fence();
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
                auto posted = session_->post( [this] { drain(); } );
                if( !posted.has_value() )
                {
                    scheduled_.store( false );
                    renderer_->remember_error( std::move( posted.error() ) );
                }
            }

            void
            drain()
            {
                // Re-arm before draining: a notify that lands mid-drain
                // schedules a follow-up drain instead of being lost; the worst
                // case is one extra drain that finds an empty queue.
                scheduled_.store( false );
                while( auto item = subscription_.try_pop_item() )
                {
                    renderer_->consume( *item );
                }
            }

            [[nodiscard]]
            grab::Result<void>
            reactor_fence()
            {
                std::promise<void> fence;
                auto               reached = fence.get_future();
                auto posted = session_->post( [&fence] { fence.set_value(); } );
                if( !posted.has_value() )
                {
                    return posted;
                }
                reached.get();
                return {};
            }

            grab::Session*     session_;
            grab::Subscription subscription_;
            TrailRenderer*     renderer_;
            std::atomic_bool   scheduled_{ false };
    };
```

- [ ] **Step 4: Wire observation and the trail into run()**

Replace `run()` with the final version:

```cpp
    [[nodiscard]]
    grab::Result<void>
    run()
    {
        auto session = grab::Session::open();
        if( !session.has_value() )
        {
            return std::unexpected( std::move( session.error() ) );
        }

        auto overlay = ( *session )->overlay();
        if( !overlay.has_value() )
        {
            return std::unexpected( std::move( overlay.error() ) );
        }
        // Phase 1 renders in the display-global space; observed positions
        // arrive in the same space, so segments need no transform. space() is
        // queried to fail fast if that invariant ever breaks.
        auto space = ( *overlay )->space();
        if( !space.has_value() )
        {
            return std::unexpected( std::move( space.error() ) );
        }

        auto screen = grab::Screen::open();
        if( !screen.has_value() )
        {
            return std::unexpected( std::move( screen.error() ) );
        }
        auto frame = screen->display();
        if( !frame.has_value() )
        {
            return std::unexpected( std::move( frame.error() ) );
        }
        const auto width  = static_cast<std::int32_t>( frame->width );
        const auto height = static_cast<std::int32_t>( frame->height );

        TrailRenderer renderer{ **overlay };

        grab::SubscriptionScope scope;
        scope.kinds       = { grab::EventKind::MouseMove };
        auto subscription = ( *session )->watch( std::move( scope ) );
        if( !subscription.has_value() )
        {
            return std::unexpected( std::move( subscription.error() ) );
        }

        TrailPump pump{ **session, std::move( *subscription ), renderer };
        pump.install();

        auto observing = ( *session )->start_observation();
        if( !observing.has_value() )
        {
            return std::unexpected( std::move( observing.error() ) );
        }

        auto input = grab::Input::open();
        if( !input.has_value() )
        {
            return std::unexpected( std::move( input.error() ) );
        }

        const auto waypoints = snake_waypoints( width, height );
        auto       placed =
            input->move( static_cast<std::int16_t>( waypoints.front().x ),
                         static_cast<std::int16_t>( waypoints.front().y ) );
        if( !placed.has_value() )
        {
            return placed;
        }
        for( std::size_t index = 1U; index < waypoints.size(); ++index )
        {
            auto swept =
                move_line( *input, waypoints[index - 1U], waypoints[index] );
            if( !swept.has_value() )
            {
                return swept;
            }
        }

        // Fence the last segments onto the screen, then hold while they fade.
        auto flushed = ( *overlay )->flush();
        if( !flushed.has_value() )
        {
            return flushed;
        }
        std::this_thread::sleep_for( endHold );

        auto stopped = pump.stop();
        if( !stopped.has_value() )
        {
            return stopped;
        }
        ( *session )->close();

        if( auto error = renderer.error(); error.has_value() )
        {
            return std::unexpected( std::move( *error ) );
        }
        return {};
    }
```

Ordering note: `pump.stop()` fences the reactor BEFORE `close()` and before `pump`/`renderer` (declared after `session`) are destroyed, so no reactor job can touch a dead object.

- [ ] **Step 5: Build**

Run: `cmake --build build -j$(nproc)`
Expected: PASS — clean build, no warnings.

- [ ] **Step 6: End-to-end smoke — blue trail pixels appear mid-run**

```bash
SCRATCH=/tmp/claude-1000/-home-gn-ws-grab-workspace/1ba3da79-43e4-48f8-81c2-7216203481e9/scratchpad
mkdir -p "$SCRATCH"
Xvfb :96 -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1
DISPLAY=:96 ./build/mouse_snake_trail &
SNAKE_PID=$!
sleep 4
DISPLAY=:96 ./build/grab capture --display --out "$SCRATCH/snake_mid.png"
wait $SNAKE_PID; echo "exit=$?"
kill $XVFB_PID
python3 - "$SCRATCH/snake_mid.png" <<'EOF'
import sys
from PIL import Image
image = Image.open(sys.argv[1]).convert("RGB")
blue = sum(1 for r, g, b in image.getdata() if b > 200 and r < 80 and g < 80)
print(f"blue_pixels={blue}")
assert blue > 100, "expected a visible blue trail"
EOF
```

Expected: `blue_pixels=` well above 100 (fading comet tail behind the cursor mid-sweep, drawn blue because the moves observe as `InjectedOther`), then `exit=0`.

- [ ] **Step 7: Regression check — full test suite still green**

Run: `ctest --test-dir build --output-on-failure -j$(nproc) | tail -3`
Expected: `100% tests passed` (same count as before this plan; examples add no tests).

- [ ] **Step 8: Commit**

```bash
git add examples/mouse_snake_trail.cpp
git commit -m "feat: observation-driven fading trail in mouse_snake_trail"
git log -1 --format='%an <%ae> / %cn <%ce>'
```

Expected: authorship shows `GNERSIS <gornersisyan4@gmail.com>` twice.

---

## Live demo (manual, after the plan)

Build the **release preset** for live runs — the default dev build carries
ASan/UBSan + coverage, whose per-pixel instrumentation drops the overlay from
60 fps to ~5 fps at 3200x2000 (frames arrive seconds apart: the trail looks
static and physical-input trails render late):

```bash
cmake --preset release
cmake --build build-release -j --target mouse_snake_trail
./build-release/examples/mouse_snake_trail
```

The snake draws blue; wiggling the physical mouse during the run draws red beside it.
