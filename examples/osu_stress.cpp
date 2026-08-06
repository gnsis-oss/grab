// osu_stress -- the VISUAL half of an OSU!-style overlay stress test.
//
// examples/sequences/osu_stress.json is a 90-step grab sequence played by
// `grab play`: it warps, moves, clicks, presses, follows curves, spins and
// scrolls across a 1920x1080 playfield. This program draws what that pointer
// is aiming at -- hit circles, approach rings, the curve routes, and one
// draggable square -- and removes each target when a click is OBSERVED on it.
//
// Two properties are the whole design, and both are anti-desync:
//
//   1. Every coordinate is READ FROM THE SAME DOCUMENT the sequence plays.
//      Nothing here is hardcoded to a playfield position. Move a target in the
//      JSON and the visual follows; there is no second copy to fall out of
//      step with the first.
//
//   2. Removal is CAUSED BY THE HIT, never scheduled alongside it. A circle
//      disappears because a button press arrived from the observation stream
//      within its radius, not because a timer expired. Retime the JSON and the
//      visuals stay correct; a circle nobody hits stays on screen, which is a
//      useful failure signal rather than a bug.
//
// It does not need the sequence to be running: started alone it places the
// field and waits, so a human can look at it.

#include "grab/event.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "grab/space.hpp"
#include "grab/watch.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>    // IWYU pragma: keep
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

// Baked in by examples/CMakeLists.txt so the default document resolves against
// the source tree rather than the working directory.
#ifndef OSU_STRESS_DEFAULT_DOCUMENT
    #define OSU_STRESS_DEFAULT_DOCUMENT "examples/sequences/osu_stress.json"
#endif

namespace
{

    // ---------------------------------------------------------------- style

    constexpr double       hitRadiusPx      = 48.0;
    constexpr double       approachRadiusPx = 78.0;
    constexpr double       flashRadiusPx    = 58.0;
    constexpr double       squareSidePx     = 90.0;
    constexpr double       squareHalfPx     = squareSidePx / 2.0;

    constexpr float        hitStrokePx      = 3.0F;
    constexpr float        approachStrokePx = 1.0F;
    constexpr float        routeStrokePx    = 1.0F;
    constexpr float        squareStrokePx   = 2.0F;
    constexpr float        flashStrokePx    = 5.0F;

    constexpr std::int32_t routeLayer       = 0;
    constexpr std::int32_t circleLayer      = 1;
    constexpr std::int32_t squareLayer      = 2;
    constexpr std::int32_t flashLayer       = 3;

    constexpr std::uint8_t opaqueChannel    = std::numeric_limits<std::uint8_t>::max();

    constexpr grab::overlay::Color hitColor{
        .r = 96U,
        .g = 208U,
        .b = 255U,
        .a = opaqueChannel,
    };
    constexpr grab::overlay::Color approachColor{
        .r = 96U,
        .g = 208U,
        .b = 255U,
        .a = 110U,
    };
    constexpr grab::overlay::Color routeColor{
        .r = 255U,
        .g = 190U,
        .b = 72U,
        .a = 130U,
    };
    constexpr grab::overlay::Color squareIdleStroke{
        .r = 255U,
        .g = 120U,
        .b = 200U,
        .a = opaqueChannel,
    };
    constexpr grab::overlay::Color squareIdleFill{
        .r = 255U,
        .g = 120U,
        .b = 200U,
        .a = 56U,
    };
    constexpr grab::overlay::Color squareCarriedStroke{
        .r = 140U,
        .g = 255U,
        .b = 170U,
        .a = opaqueChannel,
    };
    constexpr grab::overlay::Color squareCarriedFill{
        .r = 140U,
        .g = 255U,
        .b = 170U,
        .a = 110U,
    };
    constexpr grab::overlay::Color flashColor{
        .r = 255U,
        .g = 255U,
        .b = 255U,
        .a = opaqueChannel,
    };

    // The hit flash grows and fades over ~13 frames of the 60 FPS overlay
    // clock. Its lifetime is Persistent ON PURPOSE: a Ttl/Fade shape is
    // expired by the scene itself, and an explicit remove would then race that
    // expiry. Persistent + an explicit remove from the sweep keeps the
    // add/remove accounting exact, which is what the stress report needs.
    constexpr std::chrono::milliseconds flashDuration{ 220 };
    constexpr double                    flashScaleFrom   = 0.65;
    constexpr double                    flashScaleTo     = 1.60;
    constexpr double                    flashOpacityFrom = 1.0;
    constexpr double                    flashOpacityTo   = 0.0;

    // The overlay presents at 60 FPS (targetFramesPerSecond, overlay_delegate
    // .cpp): 16.7 ms per frame. A single overlay call longer than this missed
    // a frame.
    constexpr std::chrono::microseconds frameBudget{ 16'667 };

    // How often the main thread posts a flash sweep onto the reactor while it
    // holds the field open. Short enough that a flash is removed within a
    // couple of frames of its deadline, long enough not to be a spin.
    constexpr std::chrono::milliseconds sweepInterval{ 40 };
    constexpr std::chrono::seconds      defaultHold{ 45 };

    // MouseMove coalesces on overflow but MouseButtonDown/Up are NeverDrop
    // (include/grab/event_descriptor.hpp), so an overrun costs a QueueGapMarker
    // and with it a possibly-unobserved click. Ask for headroom.
    constexpr std::size_t               queueCapacity        = 4'096U;

    constexpr std::string_view          hitPrefix            = "hit-";
    constexpr std::string_view          squarePrefix         = "square-";
    constexpr std::string_view          square2Prefix        = "square2-";
    constexpr std::string_view          followOp             = "input.follow";
    constexpr std::string_view          stepsField           = "steps";
    constexpr std::string_view          idField              = "id";
    constexpr std::string_view          opField              = "op";
    constexpr std::string_view          toField              = "to";
    constexpr std::string_view          curveField           = "curve";

    constexpr std::size_t               pointArity           = 2U;
    constexpr std::size_t               pointX               = 0U;
    constexpr std::size_t               pointY               = 1U;
    constexpr std::size_t               minimumCurvePoints   = 2U;
    constexpr std::size_t               commandsPerRoute     = 2U;
    constexpr std::size_t               shapesPerHitTarget   = 2U;
    constexpr std::size_t               fieldShapeHeadroom   = 4U;
    constexpr double                    nanosecondsPerMillis = 1'000'000.0;

    constexpr int                       exitFailure          = 1;

    // ------------------------------------------------------------- document

    struct FieldPoint
    {
            double x = 0.0;
            double y = 0.0;
    };

    struct HitTarget
    {
            std::string id;
            FieldPoint  center{};
    };

    struct RouteCurve
    {
            std::string             id;
            std::vector<FieldPoint> control;
    };

    // Everything the visuals are derived from. Nothing else in this file
    // knows a playfield coordinate.
    struct Document
    {
            std::vector<HitTarget>    hits;
            std::vector<RouteCurve>   routes;
            std::string               square_id;
            std::optional<FieldPoint> square_center;
    };

    [[nodiscard]]
    bool
    is_hit_id( std::string_view id )
    {
        if( !id.starts_with( hitPrefix ) )
        {
            return false;
        }
        const std::string_view suffix = id.substr( hitPrefix.size() );
        return !suffix.empty() &&
               std::ranges::all_of( suffix,
                                    []( char character )
                                    {
                                        return character >= '0' && character <= '9';
                                    } );
    }

    [[nodiscard]]
    bool
    is_square_id( std::string_view id )
    {
        return id.starts_with( squarePrefix ) || id.starts_with( square2Prefix );
    }

    // nlohmann's object comparator is transparent only from C++14 onwards and
    // the guarantee is version-dependent; materialising the key keeps the
    // lookup unambiguous and costs nothing at document-load cadence.
    [[nodiscard]]
    nlohmann::json::const_iterator
    field_of( const nlohmann::json& node,
              std::string_view      name )
    {
        return node.find( std::string{ name } );
    }

    [[nodiscard]]
    std::optional<FieldPoint>
    read_point( const nlohmann::json& node )
    {
        if( !node.is_array() || node.size() != pointArity )
        {
            return std::nullopt;
        }
        const auto& x = node.at( pointX );
        const auto& y = node.at( pointY );
        if( !x.is_number() || !y.is_number() )
        {
            return std::nullopt;
        }
        return FieldPoint{
            .x = x.get<double>(),
            .y = y.get<double>(),
        };
    }

    [[nodiscard]]
    std::string
    read_text( const nlohmann::json& node,
               std::string_view      field )
    {
        const auto found = field_of( node, field );
        if( found == node.end() || !found->is_string() )
        {
            return std::string{};
        }
        return found->get<std::string>();
    }

    // Parses the sequence document into the three things the field is made of.
    // Steps it does not recognise are simply not drawn -- an unknown op is not
    // an error here, because the interpreter owns that judgement, not us.
    [[nodiscard]]
    grab::Result<Document>
    load_document( const std::filesystem::path& path )
    {
        std::ifstream stream{ path };
        if( !stream.is_open() )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "cannot open sequence document " + path.string() );
        }

        const auto json = nlohmann::json::parse( stream, nullptr, false );
        if( json.is_discarded() || !json.is_object() )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "malformed sequence document " + path.string() );
        }

        const auto steps = field_of( json, stepsField );
        if( steps == json.end() || !steps->is_array() )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               path.string() + " has no steps array" );
        }

        Document document;
        for( const auto& step : *steps )
        {
            if( !step.is_object() )
            {
                continue;
            }
            const std::string         id = read_text( step, idField );
            const std::string         op = read_text( step, opField );

            const auto                to = field_of( step, toField );
            std::optional<FieldPoint> target;
            if( to != step.end() )
            {
                target = read_point( *to );
            }

            if( is_hit_id( id ) && target.has_value() )
            {
                document.hits.push_back( HitTarget{
                    .id     = id,
                    .center = *target,
                } );
            }

            // The square starts where the FIRST square-* / square2-* step
            // parks the pointer, so it is under the cursor when that step's
            // press arrives.
            if( is_square_id( id ) &&
                target.has_value() &&
                !document.square_center.has_value() )
            {
                document.square_id     = id;
                document.square_center = target;
            }

            if( op == followOp )
            {
                const auto curve = field_of( step, curveField );
                if( curve ==
                    step.end() ||
                    !curve->is_array() ||
                    curve->size() < minimumCurvePoints )
                {
                    continue;
                }
                RouteCurve route;
                route.id = id;
                route.control.reserve( curve->size() );
                bool complete = true;
                for( const auto& node : *curve )
                {
                    const auto point = read_point( node );
                    if( !point.has_value() )
                    {
                        complete = false;
                        break;
                    }
                    route.control.push_back( *point );
                }
                if( complete )
                {
                    document.routes.push_back( std::move( route ) );
                }
            }
        }

        if( document.hits.empty() &&
            document.routes.empty() &&
            !document.square_center.has_value() )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               path.string() +
                                   " yielded no hit targets, routes or square" );
        }
        return document;
    }

    // ------------------------------------------------------- timed surface

    // Every overlay mutation goes through here so the stress report can say
    // exactly what the slowest call cost and how many missed the frame budget.
    // The timing brackets the call only; the mutex is never held across it.
    class Surface
    {
        public:

            // Per-verb cost, because the whole reason add_many exists is that
            // the round trip is priced per CALL rather than per shape -- a
            // claim you can only check by pricing the verbs separately.
            struct OpStat
            {
                    std::string              kind;
                    std::uint64_t            calls = 0U;
                    std::chrono::nanoseconds slowest{ 0 };
                    std::chrono::nanoseconds total{ 0 };
            };

            struct Metrics
            {
                    std::uint64_t            calls          = 0U;
                    std::uint64_t            shapes_added   = 0U;
                    std::uint64_t            shapes_removed = 0U;
                    std::uint64_t            over_budget    = 0U;
                    std::chrono::nanoseconds slowest{ 0 };
                    std::string              slowest_call;
                    std::string              slowest_thread;
                    std::vector<OpStat>      per_op;
            };

            Surface( grab::Overlay&  overlay,
                     std::thread::id main_thread ) noexcept :
                overlay_{ &overlay },
                main_thread_{ main_thread }
            {
            }

            Surface( const Surface& ) = delete;
            Surface&
            operator=( const Surface& ) = delete;
            Surface( Surface&& )        = delete;
            Surface&
            operator=( Surface&& ) = delete;
            ~Surface()             = default;

            [[nodiscard]]
            grab::Result<grab::CoordinateSpaceId>
            space()
            {
                return timed<grab::CoordinateSpaceId>( "space",
                                                       std::string{},
                                                       0U,
                                                       0U,
                                                       [this]
                                                       {
                                                           return overlay_->space();
                                                       } );
            }

            [[nodiscard]]
            grab::Result<std::vector<grab::overlay::ShapeId>>
            add_many( std::span<grab::overlay::Shape> shapes )
            {
                const auto count = static_cast<std::uint64_t>( shapes.size() );
                return timed<std::vector<grab::overlay::ShapeId>>(
                    "add_many",
                    " of " + std::to_string( shapes.size() ) + " shapes",
                    count,
                    0U,
                    [this, shapes]
                    {
                        return overlay_->add_many( shapes );
                    }
                );
            }

            [[nodiscard]]
            grab::Result<grab::overlay::ShapeId>
            add( grab::overlay::Shape shape )
            {
                return timed<grab::overlay::ShapeId>(
                    "add",
                    std::string{},
                    1U,
                    0U,
                    [this, shape = std::move( shape )]() mutable
                    {
                        return overlay_->add( std::move( shape ) );
                    }
                );
            }

            [[nodiscard]]
            grab::Result<void>
            update( grab::overlay::ShapeId id,
                    grab::overlay::Shape   shape )
            {
                return timed<void>( "update",
                                    std::string{},
                                    0U,
                                    0U,
                                    [this, id, shape = std::move( shape )]() mutable
                                    {
                                        return overlay_->update( id,
                                                                 std::move( shape ) );
                                    } );
            }

            [[nodiscard]]
            grab::Result<void>
            remove( grab::overlay::ShapeId id )
            {
                return timed<void>( "remove",
                                    std::string{},
                                    0U,
                                    1U,
                                    [this, id]
                                    {
                                        return overlay_->remove( id );
                                    } );
            }

            [[nodiscard]]
            grab::Result<void>
            flush()
            {
                return timed<void>( "flush",
                                    std::string{},
                                    0U,
                                    0U,
                                    [this]
                                    {
                                        return overlay_->flush();
                                    } );
            }

            [[nodiscard]]
            grab::Result<void>
            capture_pointer()
            {
                return timed<void>( "capture_pointer",
                                    std::string{},
                                    0U,
                                    0U,
                                    [this]
                                    {
                                        return overlay_->capture_pointer();
                                    } );
            }

            [[nodiscard]]
            grab::Result<void>
            release_pointer()
            {
                return timed<void>( "release_pointer",
                                    std::string{},
                                    0U,
                                    0U,
                                    [this]
                                    {
                                        return overlay_->release_pointer();
                                    } );
            }

            [[nodiscard]]
            Metrics
            metrics() const
            {
                const std::scoped_lock lock{ mutex_ };
                return metrics_;
            }

        private:

            template<typename T,
                     typename Operation>
            [[nodiscard]]
            grab::Result<T>
            timed( std::string_view kind,
                   std::string      detail,
                   std::uint64_t    added,
                   std::uint64_t    removed,
                   Operation&&      operation )
            {
                const auto started = std::chrono::steady_clock::now();
                auto       result  = std::forward<Operation>( operation )();
                const auto elapsed = std::chrono::steady_clock::now() - started;
                note( kind,
                      std::move( detail ),
                      elapsed,
                      added,
                      removed,
                      result.has_value() );
                return result;
            }

            void
            note( std::string_view         kind,
                  std::string              detail,
                  std::chrono::nanoseconds elapsed,
                  std::uint64_t            added,
                  std::uint64_t            removed,
                  bool                     succeeded )
            {
                const bool on_main = std::this_thread::get_id() == main_thread_;
                const std::scoped_lock lock{ mutex_ };
                ++metrics_.calls;
                if( succeeded )
                {
                    metrics_.shapes_added   += added;
                    metrics_.shapes_removed += removed;
                }
                if( elapsed > frameBudget )
                {
                    ++metrics_.over_budget;
                }
                if( elapsed > metrics_.slowest )
                {
                    metrics_.slowest        = elapsed;
                    metrics_.slowest_call   = std::string{ kind } + detail;
                    metrics_.slowest_thread = on_main ? "main" : "reactor";
                }

                auto stat = std::ranges::find_if( metrics_.per_op,
                                                  [kind]( const OpStat& candidate )
                                                  {
                                                      return candidate.kind == kind;
                                                  } );
                if( stat == metrics_.per_op.end() )
                {
                    metrics_.per_op.push_back( OpStat{
                        .kind    = std::string{ kind },
                        .calls   = 0U,
                        .slowest = std::chrono::nanoseconds::zero(),
                        .total   = std::chrono::nanoseconds::zero(),
                    } );
                    stat = std::prev( metrics_.per_op.end() );
                }
                ++stat->calls;
                stat->total   += elapsed;
                stat->slowest  = std::max( stat->slowest, elapsed );
            }

            grab::Overlay*     overlay_;
            std::thread::id    main_thread_;
            mutable std::mutex mutex_;
            Metrics            metrics_;
    };

    // ------------------------------------------------------ pointer capture

    // capture_pointer()/release_pointer() are for exactly this: a modal tool
    // that draws from the pointer. The two rules from overlay.hpp are enforced
    // here rather than at the call sites.
    //
    //   * ARM WHEN THE TOOL BECOMES ARMED, NEVER AT BUTTON-PRESS. The carry
    //     tool becomes armed when the pointer enters the square, which is
    //     strictly before the press that picks it up. Grabbing at the press
    //     would be too late: that press has already been delivered elsewhere.
    //
    //   * THE CALLER OWNS THE CAPTURE. A pointer grab that outlives its owner
    //     freezes the whole desktop, so the release is in a destructor, is
    //     unconditional, and cannot throw. Every exit path -- normal return,
    //     early error return, exception, and the SIGINT/SIGTERM path, which
    //     only sets a flag and unwinds through here -- runs it.
    class PointerCapture
    {
        public:

            explicit PointerCapture( Surface& surface ) noexcept :
                surface_{ &surface }
            {
            }

            ~PointerCapture()
            {
                release_best_effort();
            }

            PointerCapture( const PointerCapture& ) = delete;
            PointerCapture&
            operator=( const PointerCapture& ) = delete;
            PointerCapture( PointerCapture&& ) = delete;
            PointerCapture&
            operator=( PointerCapture&& ) = delete;

            [[nodiscard]]
            grab::Result<void>
            arm()
            {
                const std::scoped_lock lock{ mutex_ };
                if( armed_ )
                {
                    return {};
                }
                auto captured = surface_->capture_pointer();
                if( !captured.has_value() )
                {
                    remember( captured.error() );
                    ++refused_;
                    return captured;
                }
                armed_ = true;
                ++arms_;
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            disarm()
            {
                const std::scoped_lock lock{ mutex_ };
                if( !armed_ )
                {
                    return {};
                }
                // Clear the flag FIRST: if the release reports a failure the
                // delegate has still reverted to click-through, and a retry
                // loop that kept the flag set would never let go.
                armed_ = false;
                ++disarms_;
                auto released = surface_->release_pointer();
                if( !released.has_value() )
                {
                    remember( released.error() );
                }
                return released;
            }

            [[nodiscard]]
            bool
            armed() const
            {
                const std::scoped_lock lock{ mutex_ };
                return armed_;
            }

            [[nodiscard]]
            std::uint64_t
            arms() const
            {
                const std::scoped_lock lock{ mutex_ };
                return arms_;
            }

            [[nodiscard]]
            std::uint64_t
            disarms() const
            {
                const std::scoped_lock lock{ mutex_ };
                return disarms_;
            }

            [[nodiscard]]
            std::uint64_t
            refused() const
            {
                const std::scoped_lock lock{ mutex_ };
                return refused_;
            }

            [[nodiscard]]
            std::optional<grab::Error>
            failure() const
            {
                const std::scoped_lock lock{ mutex_ };
                return failure_;
            }

        private:

            void
            remember( const grab::Error& error )
            {
                if( !failure_.has_value() )
                {
                    failure_ = error;
                }
            }

            void
            release_best_effort() noexcept
            {
                try
                {
                    const std::scoped_lock lock{ mutex_ };
                    armed_ = false;
                    [[maybe_unused]]
                    auto released = surface_->release_pointer();
                }
                catch( ... )    // NOLINT(bugprone-empty-catch)
                {
                }
            }

            Surface*                   surface_;
            mutable std::mutex         mutex_;
            bool                       armed_   = false;
            std::uint64_t              arms_    = 0U;
            std::uint64_t              disarms_ = 0U;
            std::uint64_t              refused_ = 0U;
            std::optional<grab::Error> failure_;
    };

    // ------------------------------------------------------------ the field

    class OsuField
    {
        public:

            struct Report
            {
                    std::uint64_t            hits             = 0U;
                    std::uint64_t            misses           = 0U;
                    std::uint64_t            carries          = 0U;
                    std::uint64_t            presses          = 0U;
                    std::uint64_t            releases         = 0U;
                    std::uint64_t            gaps             = 0U;
                    std::size_t              peak_live_shapes = 0U;
                    std::size_t              live_shapes      = 0U;
                    std::vector<std::string> unhit;
            };

            OsuField( Surface&                surface,
                      PointerCapture&         capture,
                      grab::CoordinateSpaceId space,
                      Document                document ) :
                surface_{ &surface },
                capture_{ &capture },
                space_{ space },
                document_{ std::move( document ) }
            {
            }

            OsuField( const OsuField& ) = delete;
            OsuField&
            operator=( const OsuField& ) = delete;
            OsuField( OsuField&& )       = delete;
            OsuField&
            operator=( OsuField&& ) = delete;
            ~OsuField()             = default;

            // Places the whole field in ONE add_many. Every mutating overlay
            // call off the reactor thread is a synchronous round trip serviced
            // on the 60 FPS frame clock and paid by this thread, and the cost
            // is per CALL rather than per shape -- so the ~55 shapes of an
            // initial field placed one at a time would stall this thread for
            // most of a second. Main thread, before the pump is installed.
            [[nodiscard]]
            grab::Result<void>
            place()
            {
                std::vector<grab::overlay::Shape> shapes;
                shapes.reserve( ( document_.hits.size() * shapesPerHitTarget ) +
                                document_.routes.size() +
                                fieldShapeHeadroom );

                for( const auto& route : document_.routes )
                {
                    shapes.push_back( route_shape( route ) );
                }
                for( const auto& hit : document_.hits )
                {
                    shapes.push_back( approach_shape( hit.center ) );
                    shapes.push_back( circle_shape( hit.center ) );
                }
                if( document_.square_center.has_value() )
                {
                    square_center_ = *document_.square_center;
                    shapes.push_back( square_shape( false ) );
                }

                auto placed = surface_->add_many( std::span{ shapes } );
                if( !placed.has_value() )
                {
                    return std::unexpected( std::move( placed.error() ) );
                }

                // Shapes went in as routes, then (ring, body) per hit target,
                // then the square: the returned ids are in that same order.
                const auto& ids  = *placed;
                std::size_t next = document_.routes.size();
                circles_.reserve( document_.hits.size() );
                for( const auto& hit : document_.hits )
                {
                    circles_.push_back( Circle{
                        .id     = hit.id,
                        .center = hit.center,
                        .ring   = ids.at( next ),
                        .body   = ids.at( next + 1U ),
                        .live   = true,
                    } );
                    next += shapesPerHitTarget;
                }
                if( document_.square_center.has_value() )
                {
                    square_ = ids.at( next );
                    ++next;
                }

                live_shapes_ = ids.size();
                peak_shapes_ = live_shapes_;
                return surface_->flush();
            }

            // Reactor thread only, from the drain.
            void
            consume( const grab::SubscriptionEvent& item )
            {
                if( std::holds_alternative<grab::QueueGapMarker>( item ) )
                {
                    // A gap means a NeverDrop event was lost: a click may have
                    // gone unobserved, so say so rather than quietly under-
                    // counting the hits.
                    ++gaps_;
                    return;
                }
                const auto& event = std::get<grab::Event>( item );
                switch( event.kind )
                {
                    case grab::EventKind::MouseMove :
                        on_move( event );
                        break;
                    case grab::EventKind::MouseButtonDown :
                        on_press( event );
                        break;
                    case grab::EventKind::MouseButtonUp :
                        on_release( event );
                        break;
                    default :
                        break;
                }
            }

            // Reactor thread only: from the drain, and from the jobs the hold
            // loop posts so the last flash still goes away when the pointer
            // has stopped moving.
            void
            sweep()
            {
                const auto now = std::chrono::steady_clock::now();
                for( std::size_t index = flashes_.size(); index > 0U; --index )
                {
                    auto& flash = flashes_.at( index - 1U );
                    if( flash.expires_at > now )
                    {
                        continue;
                    }
                    drop( flash.id );
                    flashes_.erase( flashes_.begin() +
                                    static_cast<std::ptrdiff_t>( index - 1U ) );
                }
            }

            [[nodiscard]]
            Report
            report() const
            {
                Report result;
                result.hits             = hits_;
                result.misses           = misses_;
                result.carries          = carries_;
                result.presses          = presses_;
                result.releases         = releases_;
                result.gaps             = gaps_;
                result.peak_live_shapes = peak_shapes_;
                result.live_shapes      = live_shapes_;
                for( const auto& circle : circles_ )
                {
                    if( circle.live )
                    {
                        result.unhit.push_back( circle.id );
                    }
                }
                return result;
            }

            [[nodiscard]]
            std::optional<grab::Error>
            error() const
            {
                return error_;
            }

        private:

            struct Circle
            {
                    std::string            id;
                    FieldPoint             center{};
                    grab::overlay::ShapeId ring{};
                    grab::overlay::ShapeId body{};
                    bool                   live = false;
            };

            struct Flash
            {
                    grab::overlay::ShapeId                id{};
                    std::chrono::steady_clock::time_point expires_at{};
            };

            [[nodiscard]]
            grab::SpacePoint
            at( FieldPoint point ) const
            {
                return grab::SpacePoint{
                    .x     = point.x,
                    .y     = point.y,
                    .space = space_,
                };
            }

            [[nodiscard]]
            grab::overlay::Shape
            circle_shape( FieldPoint center ) const
            {
                return grab::overlay::Shape{
                    .geometry =
                        grab::overlay::Ellipse{
                                               .center   = at( center ),
                                               .radius_x = hitRadiusPx,
                                               .radius_y = hitRadiusPx,
                                               },
                    .stroke =
                        grab::overlay::StrokeStyle{
                                               .color    = hitColor,
                                               .width_px = hitStrokePx,
                                               },
                    .fill     = std::nullopt,
                    .lifetime = grab::overlay::Persistent{},
                    .band     = grab::overlay::Band::Annotation,
                    .z        = circleLayer,
                };
            }

            [[nodiscard]]
            grab::overlay::Shape
            approach_shape( FieldPoint center ) const
            {
                return grab::overlay::Shape{
                    .geometry =
                        grab::overlay::Ellipse{
                                               .center   = at( center ),
                                               .radius_x = approachRadiusPx,
                                               .radius_y = approachRadiusPx,
                                               },
                    .stroke =
                        grab::overlay::StrokeStyle{
                                               .color    = approachColor,
                                               .width_px = approachStrokePx,
                                               },
                    .fill     = std::nullopt,
                    .lifetime = grab::overlay::Persistent{},
                    .band     = grab::overlay::Band::Annotation,
                    .z        = circleLayer,
                };
            }

            // The requested mouse-line: the exact route the cursor will take.
            // `input.follow` samples its whole control list as one Bezier
            // (execute.hpp -> Curve::sample -> de Casteljau), and the raster
            // flattens BezierTo by prepending the current point and running
            // the same de Casteljau -- so MoveTo(p0) + BezierTo(p1..pn) draws
            // the walked path, not an approximation of it.
            [[nodiscard]]
            grab::overlay::Shape
            route_shape( const RouteCurve& route ) const
            {
                std::vector<grab::overlay::PathCommand> commands;
                commands.reserve( commandsPerRoute );
                commands.emplace_back(
                    grab::overlay::MoveTo{ .point = at( route.control.front() ) }
                );

                std::vector<grab::SpacePoint> control;
                control.reserve( route.control.size() - 1U );
                for( std::size_t index = 1U; index < route.control.size(); ++index )
                {
                    control.push_back( at( route.control.at( index ) ) );
                }
                commands.emplace_back(
                    grab::overlay::BezierTo{ .control = std::move( control ) }
                );

                return grab::overlay::Shape{
                    .geometry =
                        grab::overlay::Path{
                                            .commands = std::move( commands ),
                                            .closed   = false,
                                            },
                    .stroke =
                        grab::overlay::StrokeStyle{
                                            .color    = routeColor,
                                            .width_px = routeStrokePx,
                                            },
                    .fill     = std::nullopt,
                    .lifetime = grab::overlay::Persistent{  },
                    .band     = grab::overlay::Band::Annotation,
                    .z        = routeLayer,
                };
            }

            [[nodiscard]]
            grab::overlay::Shape
            square_shape( bool carried ) const
            {
                return grab::overlay::Shape{
                    .geometry =
                        grab::overlay::Rect{
                                            .bounds =
                                grab::SpaceRect{
                                    .x     = square_center_.x - squareHalfPx,
                                    .y     = square_center_.y - squareHalfPx,
                                    .w     = squareSidePx,
                                    .h     = squareSidePx,
                                    .space = space_,
                                }, },
                    .stroke =
                        grab::overlay::StrokeStyle{
                                            .color    = carried ? squareCarriedStroke : squareIdleStroke,
                                            .width_px = squareStrokePx,
                                            },
                    .fill =
                        grab::overlay::FillStyle{
                                            .color = carried ? squareCarriedFill : squareIdleFill,
                                            },
                    .lifetime = grab::overlay::Persistent{},
                    .band     = grab::overlay::Band::Annotation,
                    .z        = squareLayer,
                };
            }

            [[nodiscard]]
            grab::overlay::Shape
            flash_shape( FieldPoint center ) const
            {
                grab::overlay::ScaleChannel scale;
                scale.easing   = grab::overlay::Easing::OutCubic;
                scale.duration = flashDuration;
                scale.from     = flashScaleFrom;
                scale.to       = flashScaleTo;

                grab::overlay::OpacityChannel opacity;
                opacity.easing   = grab::overlay::Easing::OutQuad;
                opacity.duration = flashDuration;
                opacity.from     = flashOpacityFrom;
                opacity.to       = flashOpacityTo;

                return grab::overlay::Shape{
                    .geometry =
                        grab::overlay::Ellipse{
                                               .center   = at( center ),
                                               .radius_x = flashRadiusPx,
                                               .radius_y = flashRadiusPx,
                                               },
                    .stroke =
                        grab::overlay::StrokeStyle{
                                               .color    = flashColor,
                                               .width_px = flashStrokePx,
                                               },
                    .fill      = std::nullopt,
                    .lifetime  = grab::overlay::Persistent{},
                    .band      = grab::overlay::Band::Annotation,
                    .z         = flashLayer,
                    .animation = grab::overlay::AnimationSpec{
                                               .scale   = scale,
                                               .opacity = opacity,
                                               },
                };
            }

            [[nodiscard]]
            bool
            square_contains( FieldPoint point ) const
            {
                return document_.square_center.has_value() &&
                       point.x >=
                       square_center_.x -
                       squareHalfPx &&
                       point.x <=
                       square_center_.x +
                       squareHalfPx &&
                       point.y >=
                       square_center_.y -
                       squareHalfPx &&
                       point.y <=
                       square_center_.y +
                       squareHalfPx;
            }

            void
            remember( grab::Error error )
            {
                if( !error_.has_value() )
                {
                    error_ = std::move( error );
                }
            }

            void
            drop( grab::overlay::ShapeId id )
            {
                auto removed = surface_->remove( id );
                if( !removed.has_value() )
                {
                    remember( std::move( removed.error() ) );
                    return;
                }
                if( live_shapes_ > 0U )
                {
                    --live_shapes_;
                }
            }

            void
            raise( grab::overlay::Shape                  shape,
                   std::chrono::steady_clock::time_point expires_at )
            {
                auto added = surface_->add( std::move( shape ) );
                if( !added.has_value() )
                {
                    remember( std::move( added.error() ) );
                    return;
                }
                ++live_shapes_;
                peak_shapes_ = std::max( peak_shapes_, live_shapes_ );
                flashes_.push_back( Flash{
                    .id         = *added,
                    .expires_at = expires_at,
                } );
            }

            void
            on_move( const grab::Event& event )
            {
                const auto* const motion =
                    std::get_if<grab::MouseMove>( &event.payload );
                if( motion == nullptr || !motion->position.has_value() )
                {
                    return;
                }
                const FieldPoint here{
                    .x = motion->position->x,
                    .y = motion->position->y,
                };
                pointer_ = here;

                if( carrying_ )
                {
                    square_center_ = FieldPoint{
                        .x = here.x - carry_offset_.x,
                        .y = here.y - carry_offset_.y,
                    };
                    auto moved = surface_->update( square_, square_shape( true ) );
                    if( !moved.has_value() )
                    {
                        remember( std::move( moved.error() ) );
                    }
                    return;
                }

                // Arm on hover-enter -- the moment the carry tool becomes
                // armed -- and never at the press, which by then has already
                // been delivered to whatever is underneath.
                arm_for_hover( square_contains( here ) );
            }

            void
            arm_for_hover( bool inside )
            {
                if( inside == capture_->armed() )
                {
                    return;
                }
                auto changed = inside ? capture_->arm() : capture_->disarm();
                if( !changed.has_value() )
                {
                    // A refused grab is not fatal to the demo: the field still
                    // draws and the sequence still plays, it just also reaches
                    // whatever is underneath. Recorded, not thrown.
                    return;
                }
            }

            void
            on_press( const grab::Event& event )
            {
                const auto* const button =
                    std::get_if<grab::MouseButton>( &event.payload );
                if( button == nullptr || !button->position.has_value() )
                {
                    return;
                }
                const FieldPoint here{
                    .x = button->position->x,
                    .y = button->position->y,
                };
                pointer_ = here;
                ++presses_;

                bool consumed = false;
                if( !carrying_ && square_contains( here ) )
                {
                    carrying_ = true;
                    consumed  = true;
                    ++carries_;
                    carry_offset_ = FieldPoint{
                        .x = here.x - square_center_.x,
                        .y = here.y - square_center_.y,
                    };
                    auto held = surface_->update( square_, square_shape( true ) );
                    if( !held.has_value() )
                    {
                        remember( std::move( held.error() ) );
                    }
                }

                if( strike( here ) )
                {
                    consumed = true;
                }
                if( !consumed )
                {
                    ++misses_;
                }
            }

            // THE removal rule: a circle goes away because a press was
            // observed inside it. No timer, no delay, no ordering assumption
            // about which step of the document produced the press.
            [[nodiscard]]
            bool
            strike( FieldPoint here )
            {
                for( auto& circle : circles_ )
                {
                    if( !circle.live )
                    {
                        continue;
                    }
                    const double dx = here.x - circle.center.x;
                    const double dy = here.y - circle.center.y;
                    if( ( dx * dx ) + ( dy * dy ) > hitRadiusPx * hitRadiusPx )
                    {
                        continue;
                    }
                    circle.live = false;
                    ++hits_;
                    drop( circle.body );
                    drop( circle.ring );
                    raise( flash_shape( circle.center ),
                           std::chrono::steady_clock::now() + flashDuration );
                    return true;
                }
                return false;
            }

            void
            on_release( const grab::Event& event )
            {
                const auto* const button =
                    std::get_if<grab::MouseButton>( &event.payload );
                if( button != nullptr && button->position.has_value() )
                {
                    pointer_ = FieldPoint{
                        .x = button->position->x,
                        .y = button->position->y,
                    };
                }
                ++releases_;
                if( !carrying_ )
                {
                    return;
                }
                carrying_    = false;
                auto dropped = surface_->update( square_, square_shape( false ) );
                if( !dropped.has_value() )
                {
                    remember( std::move( dropped.error() ) );
                }
                if( pointer_.has_value() )
                {
                    arm_for_hover( square_contains( *pointer_ ) );
                }
            }

            Surface*                   surface_;
            PointerCapture*            capture_;
            grab::CoordinateSpaceId    space_;
            Document                   document_;

            std::vector<Circle>        circles_;
            std::vector<Flash>         flashes_;
            grab::overlay::ShapeId     square_{};
            FieldPoint                 square_center_{};
            FieldPoint                 carry_offset_{};
            std::optional<FieldPoint>  pointer_;
            bool                       carrying_    = false;

            std::uint64_t              hits_        = 0U;
            std::uint64_t              misses_      = 0U;
            std::uint64_t              carries_     = 0U;
            std::uint64_t              presses_     = 0U;
            std::uint64_t              releases_    = 0U;
            std::uint64_t              gaps_        = 0U;
            std::size_t                live_shapes_ = 0U;
            std::size_t                peak_shapes_ = 0U;

            std::optional<grab::Error> error_;
    };

    // ---------------------------------------------------------------- pump

    // Notify -> post -> drain, with a shutdown fence. Same shape as
    // mouse_snake_trail's TrailPump, and for the same reason: draining on the
    // reactor keeps the subscription queue short, and MouseButtonDown is a
    // NeverDrop kind whose loss would be recorded as a queue gap.
    class FieldPump
    {
        public:

            FieldPump( grab::Session&     session,
                       grab::Subscription subscription,
                       OsuField&          field ) :
                session_{ &session },
                subscription_{ std::move( subscription ) },
                field_{ &field }
            {
            }

            FieldPump( const FieldPump& ) = delete;
            FieldPump&
            operator=( const FieldPump& ) = delete;
            FieldPump( FieldPump&& )      = delete;
            FieldPump&
            operator=( FieldPump&& ) = delete;
            ~FieldPump()             = default;

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

            // Stops event flow, then fences the reactor so no queued drain can
            // touch this object or the field after stop() returns.
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
                auto posted = session_->post(
                    [this]
                    {
                        drain();
                    }
                );
                if( !posted.has_value() )
                {
                    scheduled_.store( false );
                }
            }

            void
            drain()
            {
                // Re-arm before draining so a notify landing mid-drain
                // schedules a follow-up instead of being lost.
                scheduled_.store( false );
                while( auto item = subscription_.try_pop_item() )
                {
                    field_->consume( *item );
                }
                field_->sweep();
            }

            [[nodiscard]]
            grab::Result<void>
            reactor_fence()
            {
                std::promise<void> fence;
                auto               reached = fence.get_future();
                auto               posted  = session_->post(
                    [&fence]
                    {
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

            grab::Session*     session_;
            grab::Subscription subscription_;
            OsuField*          field_;
            std::atomic_bool   scheduled_{ false };
    };

    // ------------------------------------------------------- process wiring

    volatile std::sig_atomic_t interrupted = 0;    // NOLINT(cert-err58-cpp)

    extern "C" void
    on_signal( int )
    {
        interrupted = 1;
    }

    struct Options
    {
            std::filesystem::path      document{ OSU_STRESS_DEFAULT_DOCUMENT };
            std::optional<std::string> display;
            std::chrono::seconds       hold{ defaultHold };
            bool                       help = false;
    };

    constexpr std::string_view usageText =
        "usage: osu_stress [--document PATH] [--display :N] [--hold SECONDS]\n"
        "\n"
        "  --document PATH   sequence document to derive the field from\n"
        "  --display :N      X display to open the session on\n"
        "  --hold SECONDS    how long to keep the field up (0 = until "
        "interrupted)\n";

    [[nodiscard]]
    grab::Result<Options>
    parse_options( std::span<const char* const> arguments )
    {
        Options options;
        for( std::size_t index = 1U; index < arguments.size(); ++index )
        {
            const std::string_view argument{ arguments[index] };
            const bool             has_value = index + 1U < arguments.size();
            if( argument == "--help" || argument == "-h" )
            {
                options.help = true;
                return options;
            }
            if( argument == "--document" && has_value )
            {
                options.document = std::filesystem::path{ arguments[++index] };
                continue;
            }
            if( argument == "--display" && has_value )
            {
                options.display = std::string{ arguments[++index] };
                continue;
            }
            if( argument == "--hold" && has_value )
            {
                const std::string_view text{ arguments[++index] };
                long long              seconds = 0;
                const auto             parsed =
                    std::from_chars( text.data(), text.data() + text.size(), seconds );
                if( parsed.ec !=
                    std::errc{} ||
                    parsed.ptr !=
                    text.data() +
                    text.size() ||
                    seconds < 0 )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--hold wants a non-negative number of seconds" );
                }
                options.hold = std::chrono::seconds{ seconds };
                continue;
            }
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "unrecognised argument " + std::string{ argument } );
        }
        return options;
    }

    // Keeps the field on screen, posting a flash sweep onto the reactor so the
    // last hit's ring still goes away once the pointer stops moving. A hold of
    // zero waits for SIGINT/SIGTERM, which only sets a flag -- the unwind that
    // releases the pointer capture happens on this thread, normally.
    [[nodiscard]]
    grab::Result<void>
    hold_field( grab::Session&       session,
                OsuField&            field,
                std::chrono::seconds duration )
    {
        const auto started = std::chrono::steady_clock::now();
        const bool endless = duration == std::chrono::seconds::zero();
        while( interrupted == 0 )
        {
            if( !endless && std::chrono::steady_clock::now() - started >= duration )
            {
                break;
            }
            auto posted = session.post(
                [&field]
                {
                    field.sweep();
                }
            );
            if( !posted.has_value() )
            {
                return posted;
            }
            std::this_thread::sleep_for( sweepInterval );
        }
        return {};
    }

    [[nodiscard]]
    grab::Result<void>
    drive( grab::Session&       session,
           OsuField&            field,
           std::chrono::seconds duration )
    {
        grab::SubscriptionScope scope;
        scope.kinds = {
            grab::EventKind::MouseMove,
            grab::EventKind::MouseButtonDown,
            grab::EventKind::MouseButtonUp,
        };
        auto subscription =
            session.watch( std::move( scope ),
                           grab::QueueOptions{
                               .capacity = queueCapacity,
                               .overflow = grab::QueueOverflowPolicy::Coalesce,
                           } );
        if( !subscription.has_value() )
        {
            return std::unexpected( std::move( subscription.error() ) );
        }

        FieldPump pump{ session, std::move( *subscription ), field };
        pump.install();

        // From here every exit path must run pump.stop()'s reactor fence
        // before the pump and field die, or a queued drain could touch freed
        // memory.
        auto observing = session.start_observation();
        if( !observing.has_value() )
        {
            auto stopped = pump.stop();
            static_cast<void>( stopped );
            return observing;
        }

        auto held    = hold_field( session, field, duration );
        auto stopped = pump.stop();
        if( !held.has_value() )
        {
            return held;
        }
        return stopped;
    }

    [[nodiscard]]
    double
    to_milliseconds( std::chrono::nanoseconds value )
    {
        return static_cast<double>( value.count() ) / nanosecondsPerMillis;
    }

    void
    print_report( const OsuField::Report& field,
                  const Surface::Metrics& overlay,
                  const PointerCapture&   capture,
                  std::size_t             circle_count )
    {
        std::cout << "\nosu_stress report\n"
                  << "  shapes added          " << overlay.shapes_added << '\n'
                  << "  shapes removed        " << overlay.shapes_removed << '\n'
                  << "  peak concurrent       " << field.peak_live_shapes << '\n'
                  << "  still on screen       " << field.live_shapes << '\n'
                  << "  presses observed      " << field.presses << '\n'
                  << "  releases observed     " << field.releases << '\n'
                  << "  hits                  " << field.hits << " of " << circle_count
                  << '\n'
                  << "  misses                " << field.misses << '\n'
                  << "  square pickups        " << field.carries << '\n'
                  << "  queue gaps            " << field.gaps << '\n'
                  << "  overlay calls         " << overlay.calls << '\n'
                  << "  pointer capture       " << capture.arms() << " armed, "
                  << capture.disarms() << " released, " << capture.refused()
                  << " refused\n";

        if( const auto failure = capture.failure(); failure.has_value() )
        {
            std::cout << "  pointer capture note  " << failure->message << '\n';
        }

        std::cout << "  slowest overlay call  " << to_milliseconds( overlay.slowest )
                  << " ms  (" << overlay.slowest_call << ", " << overlay.slowest_thread
                  << " thread)\n";
        if( overlay.over_budget > 0U )
        {
            std::cout << "  OVER THE 16.7 ms FRAME BUDGET: " << overlay.over_budget
                      << " of " << overlay.calls << " overlay calls\n";
        }
        else
        {
            std::cout << "  every overlay call was inside the 16.7 ms frame budget\n";
        }

        std::cout << "  cost per overlay verb (calls, slowest ms, mean ms)\n";
        for( const auto& stat : overlay.per_op )
        {
            const double mean = stat.calls == 0U ? 0.0
                                                 : to_milliseconds( stat.total ) /
                                                       static_cast<double>( stat.calls );
            std::cout << "    " << stat.kind << "  " << stat.calls << "  "
                      << to_milliseconds( stat.slowest ) << "  " << mean << '\n';
        }

        if( field.unhit.empty() )
        {
            std::cout << "  every hit circle was struck and removed\n";
        }
        else
        {
            std::cout << "  never struck (" << field.unhit.size() << "):";
            for( const auto& id : field.unhit )
            {
                std::cout << ' ' << id;
            }
            std::cout << '\n';
        }
        std::cout.flush();
    }

    [[nodiscard]]
    grab::Result<void>
    run( const Options& options )
    {
        auto document = load_document( options.document );
        if( !document.has_value() )
        {
            return std::unexpected( std::move( document.error() ) );
        }
        const std::size_t circle_count = document->hits.size();
        std::cout << "osu_stress: " << options.document.string() << '\n'
                  << "  hit targets " << circle_count << ", routes "
                  << document->routes.size() << ", square "
                  << ( document->square_center.has_value() ? document->square_id
                                                           : std::string{ "<none>" } )
                  << '\n';

        grab::SessionOptions session_options;
        session_options.display = options.display;
        auto session            = grab::Session::open( session_options );
        if( !session.has_value() )
        {
            return std::unexpected( std::move( session.error() ) );
        }

        auto overlay = ( *session )->overlay();
        if( !overlay.has_value() )
        {
            ( *session )->close();
            return std::unexpected( std::move( overlay.error() ) );
        }

        Surface surface{ **overlay, std::this_thread::get_id() };
        auto    space = surface.space();
        if( !space.has_value() )
        {
            ( *session )->close();
            return std::unexpected( std::move( space.error() ) );
        }

        // Declared before anything that can fail below, so its destructor --
        // the unconditional pointer release -- covers every exit path.
        PointerCapture     capture{ surface };
        OsuField           field{ surface, capture, *space, std::move( *document ) };

        auto               placed = field.place();
        grab::Result<void> outcome;
        if( placed.has_value() )
        {
            outcome = drive( **session, field, options.hold );
        }
        else
        {
            outcome = std::unexpected( std::move( placed.error() ) );
        }

        // Explicit release while the session is still open; the destructor is
        // the backstop, not the mechanism.
        auto released = capture.disarm();
        ( *session )->close();

        print_report( field.report(), surface.metrics(), capture, circle_count );

        if( !outcome.has_value() )
        {
            return outcome;
        }
        if( !released.has_value() )
        {
            return released;
        }
        if( auto failed = field.error(); failed.has_value() )
        {
            return std::unexpected( std::move( *failed ) );
        }
        return {};
    }

}    // namespace

int
main( int    argc,
      char** argv )
{
    std::signal( SIGINT, on_signal );
    std::signal( SIGTERM, on_signal );

    const std::span<const char* const> arguments{
        argv,
        static_cast<std::size_t>( argc )
    };
    auto options = parse_options( arguments );
    if( !options.has_value() )
    {
        std::cerr << "osu_stress: " << options.error().message << "\n\n" << usageText;
        return exitFailure;
    }
    if( options->help )
    {
        std::cout << usageText;
        return 0;
    }

    auto result = run( *options );
    if( !result.has_value() )
    {
        std::cerr << "osu_stress: " << result.error().message << '\n';
        return exitFailure;
    }
    return 0;
}
