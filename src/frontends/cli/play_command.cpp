#include "frontends/cli/common.hpp"
#include "frontends/cli/overlay_command.hpp"
#include "frontends/cli/play_command.hpp"
#include "grab/command.hpp"
#include "grab/command_descriptor.hpp"
#include "grab/event.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/sequence.hpp"
#include "grab/sequence_types.hpp"
#include "grab/session.hpp"
#include "grab/trace.hpp"
#include "grab/watch.hpp"
#include "kernel/presentation/overlay_scene.hpp"
#include "kernel/presentation/trail_animator.hpp"
#include "kernel/scheduling/timer_thread.hpp"
#include "kernel/sequence/drive.hpp"
#include "kernel/sequence/interpreter.hpp"
#include "kernel/sequence/player.hpp"
#include "kernel/sequence/sequence.hpp"
#include "kernel/sequence/sequence_ops.hpp"
#include "kernel/support/log.hpp"
#include "kernel/support/log_tags.hpp"
#include "sequence/session_seat.hpp"
#include "sequence/unwrap.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>    // IWYU pragma: keep
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace grab::cli
{

    namespace
    {

        using Clock       = std::chrono::steady_clock;
        using OrderedJson = nlohmann::ordered_json;
        using grab::kernel::sequence::Sequence;

        constexpr std::string_view pacingFlag     = "--pacing";
        constexpr std::string_view graceFlag      = "--grace-ms";
        constexpr std::string_view dryRunFlag     = "--dry-run";
        constexpr std::string_view reportFlag     = "--report";
        constexpr std::string_view traceFlag      = "--trace";
        constexpr std::string_view flagPrefix     = "-";
        constexpr std::string_view singleStepName = "cli";

        // ── The visual-feedback flags ─────────────────────────
        //
        // Names and defaults are the standalone verbs', so the two surfaces
        // cannot disagree about what `--fade-ms 400` means. The two renames are
        // forced by the merge and nothing else: `grab trail --color` becomes
        // `--trail-color` and `--width` becomes `--trail-width`, because on a
        // command that also carries `grab feedback`'s options a bare `--color`
        // or `--width` names neither feature.
        constexpr std::string_view trailFlag         = "--trail";
        constexpr std::string_view feedbackFlag      = "--feedback";
        constexpr std::string_view trailColorFlag    = "--trail-color";
        constexpr std::string_view injectedColorFlag = "--injected-color";
        constexpr std::string_view fadeMsFlag        = "--fade-ms";
        constexpr std::string_view trailWidthFlag    = "--trail-width";
        constexpr std::string_view noClickFlag       = "--no-click";
        constexpr std::string_view noHoldFlag        = "--no-hold";
        constexpr std::string_view holdMsFlag        = "--hold-ms";
        constexpr std::string_view rippleRadiusFlag  = "--ripple-radius";

        constexpr std::size_t      colorTextLength   = 6U;
        constexpr std::size_t      hexDigitsPerByte  = 2U;
        constexpr std::size_t      redTextOffset     = 0U;
        constexpr std::size_t      greenTextOffset   = 2U;
        constexpr std::size_t      blueTextOffset    = 4U;
        constexpr std::uint8_t     hexadecimalBase   = 16U;
        constexpr std::uint8_t     decimalDigitBase  = 10U;
        constexpr std::uint8_t     alphaHexOffset    = 10U;
        constexpr char             lowerHexA         = 'a';
        constexpr char             lowerHexF         = 'f';
        constexpr char             upperHexA         = 'A';
        constexpr char             upperHexF         = 'F';
        constexpr char             asciiZero         = '0';
        constexpr char             asciiNine         = '9';
        constexpr std::uint8_t  opaqueChannel = std::numeric_limits<std::uint8_t>::max();

        // ── Report formatting ─────────────────────────────────
        //
        // Two decimals and a unit chosen by magnitude, computed in integers.
        // std::to_chars over a double would need a precision and a rounding
        // mode argued about; the report is read by a human deciding what to
        // optimise, and 8.42 s tells them what 8.4213977 s does not.
        constexpr std::int64_t  nanosecondsPerMicrosecond = 1'000;
        constexpr std::int64_t  nanosecondsPerMillisecond = 1'000'000;
        constexpr std::int64_t  nanosecondsPerSecond      = 1'000'000'000;
        constexpr std::int64_t  hundredths                = 100;
        constexpr std::int64_t  tenths                    = 10;

        // Column stops. Left-aligned, because the names are the thing being
        // scanned and a right-aligned name column reads as ragged.
        constexpr std::size_t   nameColumn  = 4U;
        constexpr std::size_t   countColumn = 28U;
        constexpr std::size_t   totalColumn = 42U;
        constexpr std::size_t   meanColumn  = 56U;
        constexpr std::size_t   maxColumn   = 74U;

        constexpr std::uint64_t noCalls     = 0U;
        constexpr std::uint64_t oneCall     = 1U;

        [[nodiscard]]
        std::string
        format_duration( std::chrono::nanoseconds value )
        {
            const std::int64_t count   = value.count();

            std::int64_t       divisor = nanosecondsPerSecond;
            std::string_view   unit    = "s";
            if( count < nanosecondsPerMicrosecond )
            {
                // Sub-microsecond, or negative if a caller ever hands one over.
                // Printing "0.00 us" would hide both.
                return std::to_string( count ) + " ns";
            }
            if( count < nanosecondsPerMillisecond )
            {
                divisor = nanosecondsPerMicrosecond;
                unit    = "us";
            }
            else if( count < nanosecondsPerSecond )
            {
                divisor = nanosecondsPerMillisecond;
                unit    = "ms";
            }

            const std::int64_t whole    = count / divisor;
            const std::int64_t fraction = ( ( count % divisor ) * hundredths ) / divisor;

            std::string        text     = std::to_string( whole );
            text.push_back( '.' );
            if( fraction < tenths )
            {
                text.push_back( '0' );
            }
            text.append( std::to_string( fraction ) );
            text.push_back( ' ' );
            text.append( unit );
            return text;
        }

        // Pads the CURRENT line out to `column`, or appends a single space if
        // it is already past it. Measuring from the last newline is what keeps
        // the columns aligned in a report built by appending.
        void
        pad_to( std::string& text,
                std::size_t  column )
        {
            const auto        newline = text.rfind( '\n' );
            const std::size_t start   = newline == std::string::npos ? 0U : newline + 1U;
            const std::size_t width   = text.size() - start;
            if( width < column )
            {
                text.append( column - width, ' ' );
                return;
            }
            text.push_back( ' ' );
        }

        // "1 call" / "48 calls" -- the unit is a parameter because the same
        // renderer prints calls, steps and waits.
        [[nodiscard]]
        std::string
        format_count( std::uint64_t    count,
                      std::string_view singular,
                      std::string_view plural )
        {
            std::string text = std::to_string( count );
            text.push_back( ' ' );
            text.append( count == oneCall ? singular : plural );
            return text;
        }

        [[nodiscard]]
        std::vector<grab::diag::Tally>
        by_total_descending( const grab::diag::Instrument& instrument )
        {
            std::vector<grab::diag::Tally> sorted{
                instrument.tallies().begin(),
                instrument.tallies().end()
            };
            std::ranges::sort( sorted,
                               []( const grab::diag::Tally& left,
                                   const grab::diag::Tally& right )
                               {
                                   return left.total > right.total;
                               } );
            return sorted;
        }

        void
        append_tally( std::string&             text,
                      const grab::diag::Tally& tally,
                      std::string_view         singular,
                      std::string_view         plural )
        {
            text.append( nameColumn, ' ' );
            text.append( tally.name );
            pad_to( text, countColumn );
            text.append( format_count( tally.calls, singular, plural ) );
            pad_to( text, totalColumn );
            text.append( format_duration( tally.total ) );
            if( tally.calls > oneCall )
            {
                pad_to( text, meanColumn );
                text.append( "mean " );
                text.append( format_duration( tally.mean() ) );
                pad_to( text, maxColumn );
                text.append( "max " );
                text.append( format_duration( tally.longest ) );
            }
            text.push_back( '\n' );
        }

        [[nodiscard]]
        const grab::diag::Tally*
        find_tally( const grab::diag::Instrument& instrument,
                    std::string_view              name )
        {
            for( const auto& tally : instrument.tallies() )
            {
                if( tally.name == name )
                {
                    return &tally;
                }
            }
            return nullptr;
        }

        // A day. The bound exists so that grace * depth cannot overflow the
        // nanosecond duration the plan is accumulated in.
        constexpr std::uint64_t maximumGraceMs = 86'400'000U;

        void
        print_play_usage()
        {
            ( void )std::fputs(
                "usage: grab play SEQUENCE.json "
                "[--pacing strict|grace|precise] [--grace-ms N]\n"
                "                 [--dry-run] [--report PATH.jsonl] [--trace]\n"
                "                 [--trail] [--trail-color RRGGBB] "
                "[--injected-color RRGGBB]\n"
                "                 [--fade-ms N] [--trail-width F]\n"
                "                 [--feedback] [--no-click] [--no-hold] "
                "[--hold-ms N] [--ripple-radius PX]\n",
                stderr
            );
        }

        [[nodiscard]]
        std::string_view
        neutralization_name( grab::NeutralizationOutcome outcome ) noexcept
        {
            switch( outcome )
            {
                case grab::NeutralizationOutcome::NotAttempted :
                    return "not_attempted";
                case grab::NeutralizationOutcome::NothingHeld :
                    return "nothing_held";
                case grab::NeutralizationOutcome::Released :
                    return "released";
                case grab::NeutralizationOutcome::Failed :
                    return "failed";
            }
            return "not_attempted";
        }

        // The commit column of the per-step Receipt. The Player reports a
        // status rather than a commit, so this is a projection: a step that
        // succeeded committed, and one that never entered did not.
        [[nodiscard]]
        grab::CommitStatus
        commit_of( grab::sequence::StepStatus status ) noexcept
        {
            switch( status )
            {
                case grab::sequence::StepStatus::Succeeded :
                    return grab::CommitStatus::Committed;
                case grab::sequence::StepStatus::Failed :
                case grab::sequence::StepStatus::Pending :
                case grab::sequence::StepStatus::Ready :
                case grab::sequence::StepStatus::Running :
                case grab::sequence::StepStatus::Skipped :
                case grab::sequence::StepStatus::Count :
                    break;
            }
            return grab::CommitStatus::FailedBeforeCommit;
        }

        // The seat, its overlay bookkeeping and the capture path live in the
        // library now (src/sequence/session_seat.hpp), shared with the public
        // sequence API. `grab play` keeps driving the exact same seat.
        using grab::sequence::SessionSeat;

        [[nodiscard]]
        grab::Result<std::uint64_t>
        parse_milliseconds( std::string_view text )
        {
            std::uint64_t     value = 0U;
            const auto* const begin = text.data();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            const auto* const end    = begin + text.size();
            const auto        parsed = std::from_chars( begin, end, value );
            if( text.empty() ||
                parsed.ec !=
                std::errc{} ||
                parsed.ptr !=
                end ||
                value > maximumGraceMs )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "--grace-ms must be a whole number of "
                                   "milliseconds in range 0..86400000" );
            }
            return value;
        }

        // ── Style-flag values ─────────────────────────────────
        //
        // Same grammar and the same rejections as the standalone verbs: RRGGBB
        // hex, a positive millisecond count for a fade, a positive finite width
        // and a nonnegative one for the gesture thresholds and the ripple.

        [[nodiscard]]
        std::optional<std::uint8_t>
        hex_digit( char value ) noexcept
        {
            if( value >= asciiZero && value <= asciiNine )
            {
                return static_cast<std::uint8_t>( value - asciiZero );
            }
            if( value >= lowerHexA && value <= lowerHexF )
            {
                return static_cast<std::uint8_t>( value - lowerHexA + alphaHexOffset );
            }
            if( value >= upperHexA && value <= upperHexF )
            {
                return static_cast<std::uint8_t>( value - upperHexA + alphaHexOffset );
            }
            return std::nullopt;
        }

        [[nodiscard]]
        grab::Result<grab::overlay::Color>
        parse_color( std::string_view text,
                     std::string_view flag )
        {
            if( text.size() != colorTextLength )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   std::string{ flag } + " must match RRGGBB" );
            }
            const auto channel = [text]( std::size_t offset )
            {
                const auto pair = text.substr( offset, hexDigitsPerByte );
                const auto high = hex_digit( pair.front() );
                const auto low  = hex_digit( pair.back() );
                return high.has_value() && low.has_value()
                         ? std::optional<std::uint8_t>{ static_cast<std::uint8_t>(
                               ( *high * hexadecimalBase ) + *low
                           ) }
                         : std::nullopt;
            };
            const auto red   = channel( redTextOffset );
            const auto green = channel( greenTextOffset );
            const auto blue  = channel( blueTextOffset );
            if( !red.has_value() || !green.has_value() || !blue.has_value() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   std::string{ flag } + " must match RRGGBB" );
            }
            return grab::overlay::Color{
                .r = *red,
                .g = *green,
                .b = *blue,
                .a = opaqueChannel,
            };
        }

        [[nodiscard]]
        grab::Result<std::chrono::milliseconds>
        parse_positive_duration( std::string_view text,
                                 std::string_view flag )
        {
            std::chrono::milliseconds::rep value{};
            const auto* const              begin = text.begin();
            const auto* const              end   = text.end();
            const auto parsed = std::from_chars( begin, end, value, decimalDigitBase );
            if( text.empty() ||
                parsed.ec !=
                std::errc{} ||
                parsed.ptr !=
                end ||
                value <= 0 )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   std::string{ flag } +
                                       " must be a positive millisecond count" );
            }
            return std::chrono::milliseconds{ value };
        }

        [[nodiscard]]
        grab::Result<std::chrono::milliseconds>
        parse_nonnegative_duration( std::string_view text,
                                    std::string_view flag )
        {
            std::chrono::milliseconds::rep value{};
            const auto* const              begin = text.begin();
            const auto* const              end   = text.end();
            const auto parsed = std::from_chars( begin, end, value, decimalDigitBase );
            if( text.empty() ||
                parsed.ec !=
                std::errc{} ||
                parsed.ptr !=
                end ||
                value < 0 )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   std::string{ flag } +
                                       " must be a nonnegative millisecond count" );
            }
            return std::chrono::milliseconds{ value };
        }

        [[nodiscard]]
        grab::Result<double>
        parse_nonnegative_number( std::string_view text,
                                  std::string_view flag )
        {
            double            value{};
            const auto* const begin  = text.begin();
            const auto* const end    = text.end();
            const auto        parsed = std::from_chars( begin, end, value );
            if( text.empty() ||
                parsed.ec !=
                std::errc{} ||
                parsed.ptr !=
                end ||
                !std::isfinite( value ) ||
                value < 0.0 )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   std::string{ flag } +
                                       " must be a finite nonnegative number" );
            }
            return value;
        }

        [[nodiscard]]
        grab::Result<float>
        parse_trail_width( std::string_view text,
                           std::string_view flag )
        {
            auto value = parse_nonnegative_number( text, flag );
            if( !value.has_value() )
            {
                return std::unexpected( std::move( value.error() ) );
            }
            if( *value <=
                0.0 ||
                *value > static_cast<double>( std::numeric_limits<float>::max() ) )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   std::string{ flag } +
                                       " must be a positive finite number" );
            }
            return static_cast<float>( *value );
        }

        // ── The interrupt path ────────────────────────────────
        //
        // SIGINT and SIGTERM during a run, turned into an ordinary unwind
        // rather than a dead process. This matters most for overlay.grab. The
        // X server does drop a pointer grab when the grabbing client's
        // connection closes, so a KILLED process is not left holding it -- but
        // a process that merely stops pumping and goes on to write its report
        // would be, and so would any embedder that outlives the run. Turning
        // the signal into player.interrupt() runs the same unwind an abort
        // does, so the capture and every held button come up first.
        volatile std::sig_atomic_t interruptRequested = 0;    // NOLINT(cert-err58-cpp)

        extern "C" void
        note_interrupt( int )
        {
            interruptRequested = 1;
        }

        using SignalHandler = void ( * )( int );

        class InterruptTrap
        {
            public:

                InterruptTrap() noexcept
                {
                    interruptRequested = 0;
                    previous_int_      = std::signal( SIGINT, note_interrupt );
                    previous_term_     = std::signal( SIGTERM, note_interrupt );
                }

                ~InterruptTrap()
                {
                    // Restored, not left installed: this is a library entry
                    // point as well as a CLI verb, and a process-wide
                    // disposition it never gives back is a side effect nobody
                    // asked for.
                    if( previous_int_ != SIG_ERR )
                    {
                        ( void )std::signal( SIGINT, previous_int_ );
                    }
                    if( previous_term_ != SIG_ERR )
                    {
                        ( void )std::signal( SIGTERM, previous_term_ );
                    }
                    interruptRequested = 0;
                }

                InterruptTrap( const InterruptTrap& ) = delete;
                InterruptTrap&
                operator=( const InterruptTrap& ) = delete;
                InterruptTrap( InterruptTrap&& )  = delete;
                InterruptTrap&
                operator=( InterruptTrap&& ) = delete;

            private:

                SignalHandler previous_int_{ SIG_ERR };
                SignalHandler previous_term_{ SIG_ERR };
        };

        // ── --trail and --feedback ────────────────────────────
        //
        // The two visual surfaces a playback can raise, as ONE owner with ONE
        // teardown. Both are assembled out of what already exists:
        //
        //   feedback  Session::cursor_feedback(), a first-class session API
        //             returning a move-only RAII handle. Nothing here does more
        //             than hold it for the run and read its status at the end.
        //   trail     the assembly `grab trail` uses -- watch({MouseMove})
        //             feeding kernel::presentation::TrailAnimator through an
        //             OverlayScene whose Upsert deltas become add_many calls.
        //
        // WHERE THE TRAIL IS DRAINED IS THE ONE REAL CHOICE HERE. `grab trail`
        // has nothing else to do, so it drains on the session's reactor thread
        // from the subscription's notify callback. `grab play` does have
        // something else to do: its drive loop owns that same overlay for every
        // overlay.* step. Draining from the loop instead keeps every mutation
        // of the surface on ONE thread and needs no notify callback that could
        // fire into a half-torn-down animator. The cadence comes free -- the
        // loop wakes once per waypoint, which is the 4-6 ms a document paces
        // its motion at, and never blocks longer than maximumWaitMs.
        //
        // BOTH HANDLES ARE RELEASED ON EVERY EXIT PATH, for the same reason
        // release_outstanding() exists: stop() is called explicitly on the
        // normal, failed and signalled paths, and the destructor is the
        // backstop for a throw between them. A feedback presenter that outlives
        // its session is a leak; a subscription left installed holds an
        // observation reference the session cannot retire.
        class VisualFeedback final
        {
            public:

                VisualFeedback() = default;

                ~VisualFeedback()
                {
                    stop();
                }

                VisualFeedback( const VisualFeedback& ) = delete;
                VisualFeedback&
                operator=( const VisualFeedback& ) = delete;
                VisualFeedback( VisualFeedback&& ) = delete;
                VisualFeedback&
                operator=( VisualFeedback&& ) = delete;

                // All-or-nothing: a partial start is torn down before the error
                // is returned, so a failed --feedback cannot leave a live trail
                // subscription behind it.
                [[nodiscard]]
                grab::Result<void>
                start( grab::Session&     session,
                       grab::Overlay&     overlay,
                       const PlayOptions& options )
                {
                    session_    = &session;
                    overlay_    = &overlay;

                    auto opened = open( options );
                    if( !opened.has_value() )
                    {
                        stop();
                        return opened;
                    }
                    return {};
                }

                // One drain of the observation queue into the animator, then
                // one add_many for everything it produced. Cheap when idle: an
                // empty queue is one mutex acquisition and an empty vector.
                void
                pump()
                {
                    if( !animator_.has_value() )
                    {
                        return;
                    }
                    while( auto item = subscription_.try_pop_item() )
                    {
                        animator_->consume( *item );
                    }
                    flush_pending();
                }

                void
                stop() noexcept
                {
                    if( stopped_ )
                    {
                        return;
                    }
                    stopped_ = true;
                    try
                    {
                        // The tail of the run, drawn rather than dropped: the
                        // last waypoints of the last motion command are still
                        // in the queue when the loop ends.
                        pump();
                        if( feedback_.has_value() )
                        {
                            auto status = feedback_->status();
                            if( !status.has_value() )
                            {
                                remember( std::move( status.error() ) );
                            }
                            feedback_.reset();
                        }
                        if( scene_.has_value() )
                        {
                            scene_->set_delta_sink( {} );
                        }
                        animator_.reset();
                        scene_.reset();
                        subscription_ = grab::Subscription{};
                        if( observing_ && session_ != nullptr )
                        {
                            session_->stop_observation();
                            observing_ = false;
                        }
                    }
                    catch( ... )    // NOLINT(bugprone-empty-catch)
                    {
                        // Teardown is the last thing that runs; there is nobody
                        // left to report to, and throwing out of a noexcept
                        // teardown would abort the process instead.
                    }
                }

                // The first thing that went wrong while drawing, if anything
                // did. A requested feature that silently drew nothing is the
                // failure this exists to make visible.
                [[nodiscard]]
                const std::optional<grab::Error>&
                error() const noexcept
                {
                    return error_;
                }

            private:

                [[nodiscard]]
                grab::Result<void>
                open( const PlayOptions& options )
                {
                    if( options.trail )
                    {
                        auto started = start_trail( options.trail_style );
                        if( !started.has_value() )
                        {
                            return started;
                        }
                    }
                    if( options.feedback )
                    {
                        auto handle =
                            session_->cursor_feedback( options.feedback_style );
                        if( !handle.has_value() )
                        {
                            return std::unexpected( std::move( handle.error() ) );
                        }
                        feedback_.emplace( std::move( *handle ) );
                    }
                    return {};
                }

                [[nodiscard]]
                grab::Result<void>
                start_trail( const OverlayTrailOptions& style )
                {
                    scene_.emplace(
                        []
                        {
                            return std::chrono::duration_cast<std::chrono::milliseconds>(
                                Clock::now().time_since_epoch()
                            );
                        }
                    );
                    scene_->set_delta_sink(
                        [this]( const grab::overlay::SceneDelta& delta )
                        {
                            forward( delta );
                        }
                    );
                    animator_.emplace( *scene_,
                                       grab::kernel::presentation::TrailStyle{
                                           .physical = style.physical_color,
                                           .injected = style.injected_color,
                                           .fade     = style.fade,
                                           .width_px = style.width_px,
                                       } );

                    grab::SubscriptionScope scope;
                    scope.kinds  = { grab::EventKind::MouseMove };
                    auto watched = session_->watch( std::move( scope ) );
                    if( !watched.has_value() )
                    {
                        return std::unexpected( std::move( watched.error() ) );
                    }
                    subscription_ = std::move( *watched );

                    // Nothing arrives without this. cursor_feedback() acquires
                    // its own observation reference; a raw watch() does not, so
                    // --trail alone would subscribe to a stream nobody started.
                    auto observed = session_->start_observation();
                    if( !observed.has_value() )
                    {
                        return std::unexpected( std::move( observed.error() ) );
                    }
                    observing_ = true;
                    return {};
                }

                // Batched, not one add per segment: every mutating Overlay call
                // is a round trip to the reactor thread priced per CALL, so a
                // producer adding one shape at a time stalls itself. A
                // non-Upsert delta (an expiry, a clear) is the scene saying the
                // batch it was accumulating is complete.
                void
                forward( const grab::overlay::SceneDelta& delta )
                {
                    const auto* const upsert =
                        std::get_if<grab::overlay::Upsert>( &delta.change );
                    if( upsert != nullptr )
                    {
                        pending_.push_back( upsert->record.shape );
                        return;
                    }
                    flush_pending();
                }

                void
                flush_pending()
                {
                    if( pending_.empty() || overlay_ == nullptr )
                    {
                        pending_.clear();
                        return;
                    }
                    auto added = overlay_->add_many( pending_ );
                    pending_.clear();
                    if( !added.has_value() )
                    {
                        remember( std::move( added.error() ) );
                    }
                }

                void
                remember( grab::Error error )
                {
                    if( !error_.has_value() )
                    {
                        error_ = std::move( error );
                    }
                }

                grab::Session* session_{ nullptr };
                grab::Overlay* overlay_{ nullptr };
                std::optional<grab::kernel::presentation::OverlayScene>  scene_{};
                std::optional<grab::kernel::presentation::TrailAnimator> animator_{};
                grab::Subscription                                       subscription_{};
                std::optional<grab::CursorFeedback>                      feedback_{};
                std::vector<grab::overlay::Shape>                        pending_{};
                std::optional<grab::Error>                               error_{};
                bool observing_{ false };
                bool stopped_{ false };
        };

        // ── The machine-readable half ─────────────────────
        //
        // Everything --trace prints, as one JSONL object. The pretty report is
        // for a human deciding what to optimise; this is for whatever answers
        // "did that change help" over a hundred runs. Both are built from the
        // same tallies, so they cannot drift apart.
        [[nodiscard]]
        OrderedJson
        tallies_json( const grab::diag::Instrument& instrument )
        {
            OrderedJson array = OrderedJson::array();
            for( const auto& tally : instrument.tallies() )
            {
                array.push_back( OrderedJson{
                    {    "name", std::string{ tally.name }},
                    {   "calls",               tally.calls},
                    {"total_ns",       tally.total.count()},
                    {  "min_ns",    tally.shortest.count()},
                    {  "max_ns",     tally.longest.count()},
                    { "mean_ns",      tally.mean().count()},
                } );
            }
            return array;
        }

        [[nodiscard]]
        std::string
        trace_record( const std::string& run,
                      const RunTrace&    trace )
        {
            const OrderedJson scheduling{
                {             "arms",              trace.schedule.arms},
                {          "cancels",           trace.schedule.cancels},
                {            "fires",             trace.schedule.fires},
                {           "drains",            trace.schedule.drains},
                {  "spurious_drains",    trace.schedule.spuriousDrains},
                {    "nearer_rearms",      trace.schedule.nearerRearms},
                {    "deepest_armed",      trace.schedule.deepestArmed},
                {      "deepest_due",        trace.schedule.deepestDue},
                {"armed_depth_total",   trace.schedule.armedDepthTotal},
                {  "due_depth_total",     trace.schedule.dueDepthTotal},
                {          "tallies", tallies_json( trace.scheduling )},
            };

            const OrderedJson record{
                {         "run",run                                },
                {    "sequence",                    trace.sequence},
                // The discriminator. A consumer that wants only step records
                // filters on this rather than on the absence of a key.
                {        "kind",                           "trace"},
                {       "steps",                       trace.steps},
                {  "elapsed_ns",             trace.elapsed.count()},
                {  "planned_ns",             trace.planned.count()},
                { "unestimated",                 trace.unestimated},
                {     "load_ns",                trace.load.count()},
                {"load_tallies", tallies_json( trace.load_phases )},
                { "run_tallies",         tallies_json( trace.run )},
                {"pump_tallies",        tallies_json( trace.pump )},
                {  "scheduling",                        scheduling},
                {         "ran",                         trace.ran},
                // True when an instrument ran out of slots. A consumer that
                // ignores this is averaging over an incomplete accounting.
                {  "overflowed",
                 trace.load_phases.overflowed() ||
                 trace.run.overflowed() ||
                 trace.pump.overflowed() ||
                 trace.scheduling.overflowed()                    },
            };
            return record.dump();
        }

    }    // namespace

    grab::Result<PlayOptions>
    parse_play_options( std::span<const std::string_view> args )
    {
        PlayOptions                     options;
        bool                            has_document = false;

        // Which STYLE flags were seen, so a style without its feature can be
        // rejected by name after the whole line has been read. Deciding at the
        // point of the flag would make `--fade-ms 400 --trail` an error and
        // `--trail --fade-ms 400` a success, which is an ordering rule nobody
        // can guess.
        std::optional<std::string_view> trail_style_flag;
        std::optional<std::string_view> feedback_style_flag;
        bool                            click_enabled = true;
        bool                            hold_enabled  = true;

        auto                            current       = args.begin();
        while( current != args.end() )
        {
            const std::string_view argument = *current;
            ++current;

            if( argument == dryRunFlag )
            {
                options.dry_run = true;
                continue;
            }
            if( argument == traceFlag )
            {
                options.trace = true;
                continue;
            }
            if( argument == trailFlag )
            {
                options.trail = true;
                continue;
            }
            if( argument == feedbackFlag )
            {
                options.feedback = true;
                continue;
            }
            if( argument == noClickFlag )
            {
                click_enabled       = false;
                feedback_style_flag = noClickFlag;
                continue;
            }
            if( argument == noHoldFlag )
            {
                hold_enabled        = false;
                feedback_style_flag = noHoldFlag;
                continue;
            }
            if( argument == trailColorFlag || argument == injectedColorFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       std::string{ argument } + " requires a value" );
                }
                auto color = parse_color( *current, argument );
                if( !color.has_value() )
                {
                    return std::unexpected( std::move( color.error() ) );
                }
                if( argument == trailColorFlag )
                {
                    options.trail_style.physical_color = *color;
                }
                else
                {
                    options.trail_style.injected_color = *color;
                }
                trail_style_flag = argument;
                ++current;
                continue;
            }
            if( argument == fadeMsFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--fade-ms requires a value" );
                }
                auto fade = parse_positive_duration( *current, fadeMsFlag );
                if( !fade.has_value() )
                {
                    return std::unexpected( std::move( fade.error() ) );
                }
                options.trail_style.fade = *fade;
                trail_style_flag         = fadeMsFlag;
                ++current;
                continue;
            }
            if( argument == trailWidthFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--trail-width requires a value" );
                }
                auto width = parse_trail_width( *current, trailWidthFlag );
                if( !width.has_value() )
                {
                    return std::unexpected( std::move( width.error() ) );
                }
                options.trail_style.width_px = *width;
                trail_style_flag             = trailWidthFlag;
                ++current;
                continue;
            }
            if( argument == holdMsFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--hold-ms requires a value" );
                }
                auto hold = parse_nonnegative_duration( *current, holdMsFlag );
                if( !hold.has_value() )
                {
                    return std::unexpected( std::move( hold.error() ) );
                }
                options.feedback_style.thresholds.hold = *hold;
                feedback_style_flag                    = holdMsFlag;
                ++current;
                continue;
            }
            if( argument == rippleRadiusFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--ripple-radius requires a value" );
                }
                auto radius = parse_nonnegative_number( *current, rippleRadiusFlag );
                if( !radius.has_value() )
                {
                    return std::unexpected( std::move( radius.error() ) );
                }
                if( options.feedback_style.click.has_value() )
                {
                    options.feedback_style.click->radius_px = *radius;
                }
                feedback_style_flag = rippleRadiusFlag;
                ++current;
                continue;
            }
            if( argument == pacingFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--pacing requires a value" );
                }
                const auto mode = grab::sequence::pacing_mode_from_name( *current );
                if( !mode.has_value() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--pacing must be strict, grace or precise" );
                }
                options.pacing = *mode;
                ++current;
                continue;
            }
            if( argument == graceFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--grace-ms requires a value" );
                }
                auto milliseconds = parse_milliseconds( *current );
                if( !milliseconds.has_value() )
                {
                    return std::unexpected( std::move( milliseconds.error() ) );
                }
                options.grace = std::chrono::milliseconds{
                    static_cast<std::chrono::milliseconds::rep>( *milliseconds )
                };
                ++current;
                continue;
            }
            if( argument == reportFlag )
            {
                if( current == args.end() || current->empty() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--report requires a path" );
                }
                options.report = *current;
                ++current;
                continue;
            }
            if( argument.starts_with( flagPrefix ) )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "unknown option for play: " +
                                       std::string{ argument } );
            }
            if( has_document )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "play accepts exactly one sequence document" );
            }
            options.document = argument;
            has_document     = true;
        }

        if( !has_document || options.document.empty() )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "play requires a sequence document" );
        }

        // A style flag without its feature is an ERROR naming the flag that is
        // missing, not a silent no-op. `--fade-ms 400` on its own would
        // otherwise parse, run, and draw nothing, and the only evidence would
        // be an absent trail.
        if( !options.trail && trail_style_flag.has_value() )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               std::string{ *trail_style_flag } +
                                   " styles the mouse trail and needs --trail" );
        }
        if( !options.feedback && feedback_style_flag.has_value() )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               std::string{ *feedback_style_flag } +
                                   " styles the cursor feedback and needs "
                                   "--feedback" );
        }

        // Applied last, so --no-click still suppresses the ripple when
        // --ripple-radius set a radius for it earlier on the same line.
        if( !click_enabled )
        {
            options.feedback_style.click.reset();
        }
        if( !hold_enabled )
        {
            options.feedback_style.hold.reset();
        }
        return options;
    }

    grab::sequence::PacingOptions
    effective_pacing( grab::sequence::PacingOptions document,
                      const PlayOptions&            options ) noexcept
    {
        return grab::sequence::PacingOptions{
            .mode  = options.pacing.value_or( document.mode ),
            .grace = options.grace.value_or( document.grace ),
        };
    }

    grab::Result<Sequence>
    with_pacing( const Sequence&               program,
                 grab::sequence::PacingOptions pacing )
    {
        return grab::kernel::sequence::with_pacing( program, pacing );
    }

    grab::Result<Sequence>
    single_step_sequence( grab::sequence::Command command )
    {
        std::vector<grab::sequence::Step> steps;
        steps.push_back( grab::sequence::Step{
            .command = std::move( command ),
        } );
        return Sequence::build( std::move( steps ),
                                grab::sequence::PacingOptions{},
                                std::string{ singleStepName } );
    }

    std::string
    dry_run_report( const Sequence&               program,
                    grab::sequence::PacingOptions pacing )
    {
        std::string text;

        text.append( "sequence: " );
        text.append( program.name() );
        text.push_back( '\n' );

        text.append( "pacing: " );
        text.append( grab::sequence::pacing_mode_name( pacing.mode ) );
        text.append( " grace_ms=" );
        text.append( std::to_string( pacing.grace.count() ) );
        text.push_back( '\n' );

        text.append( "steps: " );
        text.append( std::to_string( program.steps().size() ) );
        text.push_back( '\n' );

        text.append( "order:" );
        for( const auto id : program.order() )
        {
            text.push_back( ' ' );
            text.append( std::to_string( id.index() ) );
        }
        text.push_back( '\n' );

        for( const auto id : program.order() )
        {
            const auto* const step = program.find( id );
            if( step == nullptr )
            {
                continue;
            }
            text.append( "step " );
            text.append( std::to_string( id.index() ) );
            text.append( " '" );
            text.append( step->label );
            text.append( "' " );
            text.append(
                grab::command_name( grab::sequence::kind_of( step->command ) )
            );
            text.append( " after=[" );
            bool first = true;
            for( const auto predecessor : step->after )
            {
                if( !first )
                {
                    text.push_back( ',' );
                }
                text.append( std::to_string( predecessor.index() ) );
                first = false;
            }
            text.append( "]\n" );
        }

        // Recomputed under the EFFECTIVE pacing, never read from the document:
        // printing the document's figure while running under --pacing grace
        // would report a plan the run will not follow.
        const auto plan = std::chrono::duration_cast<std::chrono::milliseconds>(
            grab::kernel::sequence::planned( program, pacing )
        );
        text.append( "plan: >= " );
        text.append( std::to_string( plan.count() ) );
        text.append( " ms, " );
        text.append(
            std::to_string( grab::kernel::sequence::unestimated_steps( program ) )
        );
        text.append( " steps unestimated\n" );
        return text;
    }

    void
    collect_run_tallies( const Sequence&                       program,
                         const grab::kernel::sequence::Player& player,
                         RunTrace&                             into )
    {
        for( const auto id : program.order() )
        {
            const auto* const step = program.find( id );
            if( step == nullptr || !player.entered_at( id ).has_value() )
            {
                // A step that was never entered has no wall time to attribute.
                // Recording a zero for it would move the mean of its command
                // kind towards a number nothing measured.
                continue;
            }
            into.run.record(
                grab::command_name( grab::sequence::kind_of( step->command ) ),
                player.timing_of( id ).call_duration
            );
        }
    }

    std::string
    trace_report( const RunTrace& trace )
    {
        std::string text;

        // ── Headline ──────────────────────────────────────
        text.append( trace.sequence.empty() ? "sequence" : trace.sequence );
        text.append( ": " );
        text.append( std::to_string( trace.steps ) );
        text.append( trace.steps == 1U ? " step" : " steps" );
        if( trace.ran )
        {
            text.append( " in " );
            text.append( format_duration( trace.elapsed ) );
        }
        else
        {
            text.append( ", not run" );
        }
        text.append( "  (planned >= " );
        text.append( format_duration( trace.planned ) );
        text.append( ", " );
        text.append( std::to_string( trace.unestimated ) );
        text.append( " unestimated)\n" );

        // ── load ──────────────────────────────────────────
        text.append( "\n  load" );
        pad_to( text, countColumn );
        text.append( format_count( oneCall, "call", "calls" ) );
        pad_to( text, totalColumn );
        text.append( format_duration( trace.load ) );
        text.push_back( '\n' );
        for( const auto& tally : by_total_descending( trace.load_phases ) )
        {
            append_tally( text, tally, "call", "calls" );
        }

        // ── run ───────────────────────────────────────────
        //
        // Skipped entirely on a --dry-run: nothing was played, so a section of
        // zeroes would be a table of measurements nobody made.
        if( trace.ran )
        {
            std::uint64_t played = noCalls;
            for( const auto& tally : trace.run.tallies() )
            {
                played += tally.calls;
            }

            // "wall span" is not decoration. A step that is entered and
            // completes inside ONE pump has entered_at == finished_at and
            // reads 0 here, which is correct and would otherwise look like a
            // broken instrument. Its real cost is a call cost and lives in the
            // pump section.
            text.append( "\n  run (wall span)" );
            pad_to( text, countColumn );
            text.append( format_count( played, "step", "steps" ) );
            pad_to( text, totalColumn );
            text.append( format_duration( trace.run.total() ) );
            text.push_back( '\n' );
            for( const auto& tally : by_total_descending( trace.run ) )
            {
                append_tally( text, tally, "call", "calls" );
            }

            // The Player's own phases, when it exposes them. Kept as its own
            // section rather than merged into `run`: a pump phase and a
            // command are not the same kind of thing, and summing them would
            // double-count the time a command spent inside a pump.
            if( trace.pump.tallies().begin() != trace.pump.tallies().end() )
            {
                // DELIBERATELY NO TOTAL. These phases nest -- play.pump
                // contains play.enter contains the command's own tally -- so
                // their sum is not a duration of anything, and printing it
                // beside the run's wall time would invite exactly the
                // subtraction that means nothing.
                text.append( "\n  pump (call cost, phases nest)\n" );
                for( const auto& tally : by_total_descending( trace.pump ) )
                {
                    append_tally( text, tally, "call", "calls" );
                }
            }
        }

        // ── scheduling ────────────────────────────────────
        //
        // The one section that is not purely durations, because the questions
        // it answers are not: "how many wakes delivered nothing" is a count,
        // and rendering it through a duration formatter would print a spurious
        // wake count of 3 as "3 ns".
        text.append( "\n  scheduling\n" );
        const auto* const wake =
            find_tally( trace.scheduling,
                        grab::kernel::scheduling::timer_tally::wakeLatency );
        if( wake == nullptr )
        {
            text.append( nameColumn, ' ' );
            text.append( "no deadline was ever waited on\n" );
        }
        else
        {
            text.append( nameColumn, ' ' );
            text.append( "wake latency" );
            pad_to( text, countColumn );
            text.append( format_count( wake->calls, "wait", "waits" ) );
            pad_to( text, totalColumn );
            text.append( "worst " );
            text.append( format_duration( wake->longest ) );
            pad_to( text, meanColumn );
            text.append( "mean " );
            text.append( format_duration( wake->mean() ) );
            text.push_back( '\n' );
        }

        text.append( nameColumn, ' ' );
        text.append( "spurious wakes" );
        pad_to( text, countColumn );
        text.append( std::to_string( trace.schedule.spuriousDrains ) );
        text.append( " of " );
        text.append( std::to_string( trace.schedule.drains ) );
        text.append( " drains\n" );

        text.append( nameColumn, ' ' );
        text.append( "nearer rearms" );
        pad_to( text, countColumn );
        text.append( std::to_string( trace.schedule.nearerRearms ) );
        text.append( " of " );
        text.append( std::to_string( trace.schedule.arms ) );
        text.append( " arms\n" );

        text.append( nameColumn, ' ' );
        text.append( "deepest queue" );
        pad_to( text, countColumn );
        text.append( std::to_string( trace.schedule.deepestArmed ) );
        text.append( " armed, " );
        text.append( std::to_string( trace.schedule.deepestDue ) );
        text.append( " due\n" );

        for( const auto& tally : by_total_descending( trace.scheduling ) )
        {
            if( tally.name == grab::kernel::scheduling::timer_tally::wakeLatency )
            {
                continue;
            }
            append_tally( text, tally, "call", "calls" );
        }

        // ── Overflow ──────────────────────────────────────
        //
        // An instrument that ran out of slots stopped recording names it had
        // not seen before. Saying so is not optional: a report that silently
        // omits the expensive thing is worse than no report.
        if( trace.load_phases.overflowed() ||
            trace.run.overflowed() ||
            trace.pump.overflowed() ||
            trace.scheduling.overflowed() )
        {
            text.append( "\n  ! this report is INCOMPLETE: an instrument ran out of "
                         "slots and dropped names it had not seen before\n" );
        }
        return text;
    }

    std::vector<std::string>
    report_records( const Sequence&                       program,
                    const grab::kernel::sequence::Player& player,
                    const RunTrace*                       trace )
    {
        std::vector<std::string> records;
        records.reserve( program.steps().size() );

        // Timeline origin: the first entry of the run. Emitting raw
        // steady_clock counts instead would be a number nothing outside this
        // process can interpret -- steady_clock's epoch is unspecified and
        // differs between boots.
        std::optional<Clock::time_point> origin;
        for( const auto id : program.order() )
        {
            const auto entered = player.entered_at( id );
            if( entered.has_value() && ( !origin.has_value() || *entered < *origin ) )
            {
                origin = *entered;
            }
        }

        const std::string run = player.run_id().to_string();
        for( const auto id : program.order() )
        {
            const auto* const step = program.find( id );
            if( step == nullptr )
            {
                continue;
            }
            const auto  status = player.status_of( id );
            const auto  timing = player.timing_of( id );

            OrderedJson receipt{
                {        "commit",
                 std::string{
                 grab::detail::commit_status_name.text_of( commit_of( status ),
                 "failed_before_commit" )
                 }                                                            },
                {   "retry_class",
                 std::string{ grab::detail::retry_class_name.text_of(
                 grab::kernel::sequence::retry_class_of_step( *step ),
                 "never"
                 ) }                                                          },
                // Run-level: the Player reports one neutralization outcome for
                // the whole unwind rather than one per step.
                {"neutralization",
                 std::string{ neutralization_name( player.neutralization() ) }},
            };

            OrderedJson record{
                {       "run",                         run                              },
                {  "sequence",                             std::string{ program.name() }},
                {      "step",                                                id.index()},
                {     "label",                                               step->label},
                {        "op",
                 std::string{
                 grab::command_name( grab::sequence::kind_of( step->command ) )
                 }                                                                      },
                {    "status", std::string{ grab::sequence::step_status_name( status ) }},
                {   "call_ns",                              timing.call_duration.count()},
                {"overrun_ns",                           player.overrun_of( id ).count()},
                {   "receipt",                                      std::move( receipt )},
            };
            if( timing.declared.has_value() )
            {
                record["declared_ns"] = timing.declared->count();
            }
            else
            {
                record["declared_ns"] = nullptr;
            }

            // Added, never substituted. `wait_ns` is the span the step sat
            // Ready before it was entered -- the pacing and scheduling cost
            // attributable to this step, which `call_ns` deliberately excludes
            // because it measures the body.
            const auto entered  = player.entered_at( id );
            const auto ready    = player.ready_at( id );
            const auto finished = player.finished_at( id );

            record["start_ns"]  = nullptr;
            record["wait_ns"]   = nullptr;
            record["end_ns"]    = nullptr;
            if( entered.has_value() && origin.has_value() )
            {
                record["start_ns"] = ( *entered - *origin ).count();
            }
            // A ROOT HAS NO READY INSTANT. play() admits the roots without a
            // clock -- it takes none -- so their ready_at is a
            // default-constructed time_point, and subtracting it yields the
            // entry's distance from the steady clock's epoch: days, reported
            // as a wait. Null is the honest answer, and is why the guard is
            // against the unset value rather than merely against ordering.
            constexpr Clock::time_point neverReady{};
            if( entered.has_value() &&
                ready.has_value() &&
                *ready !=
                neverReady &&
                *entered >= *ready )
            {
                record["wait_ns"] = ( *entered - *ready ).count();
            }
            if( finished.has_value() && origin.has_value() && *finished >= *origin )
            {
                record["end_ns"] = ( *finished - *origin ).count();
            }
            records.push_back( record.dump() );
        }

        if( trace != nullptr )
        {
            records.push_back( trace_record( run, *trace ) );
        }
        return records;
    }

    grab::Result<void>
    write_report( const std::string&              path,
                  const std::vector<std::string>& records )
    {
        std::ofstream stream{ std::filesystem::path{ path }, std::ios::trunc };
        if( !stream )
        {
            return grab::fail( grab::ErrorCode::DeviceInaccessible,
                               "failed to open report: " + path );
        }
        for( const auto& record : records )
        {
            stream << record << '\n';
        }
        if( !stream )
        {
            return grab::fail( grab::ErrorCode::DeviceInaccessible,
                               "failed to write report: " + path );
        }
        return {};
    }

    grab::Result<void>
    drive( grab::kernel::sequence::Player& player,
           RunTrace*                       trace,
           const PumpHook&                 on_pump )
    {
        // The pump loop itself lives in the library (kernel/sequence/drive.hpp)
        // so the public sequence API and this CLI share it. What is CLI policy
        // stays here: the signal trap is the cancellation source, and the
        // timer thread's accuracy lands in --trace's RunTrace.
        grab::kernel::sequence::DriveOptions options;
        options.on_pump   = on_pump;
        options.cancelled = []
        {
            return interruptRequested != 0;
        };
        options.cancel_reason = "the run was interrupted by a signal";
        if( trace != nullptr )
        {
            options.scheduling = &trace->scheduling;
            options.schedule   = &trace->schedule;
        }
        return grab::kernel::sequence::drive( player, options );
    }

    int
    play_program( const Sequence&                        program,
                  grab::kernel::sequence::CommandRunner& runner,
                  const PlayOptions&                     options,
                  RunTrace*                              trace,
                  const PumpHook&                        on_pump )
    {
        grab::kernel::sequence::Player player{ program, runner };
        auto                           outcome = drive( player, trace, on_pump );

        if( trace != nullptr )
        {
            trace->ran     = true;
            trace->elapsed = player.elapsed();
            collect_run_tallies( program, player, *trace );
            collect_pump_tallies( player, *trace );
        }

        bool reported = true;
        if( !options.report.empty() )
        {
            auto written =
                write_report( options.report, report_records( program, player, trace ) );
            if( !written.has_value() )
            {
                print_error( written.error().message );
                reported = false;
            }
        }

        if( !outcome.has_value() )
        {
            print_error( outcome.error().message );
            return runtimeExitCode;
        }
        if( !reported )
        {
            return runtimeExitCode;
        }
        if( player.state() != grab::sequence::PlayState::Done )
        {
            print_error( "the run did not reach the end of the document" );
            return runtimeExitCode;
        }
        return successExitCode;
    }

    grab::Result<void>
    play_single_command( grab::sequence::Command command,
                         const char*             display,
                         std::string_view        layout )
    {
        auto program = single_step_sequence( std::move( command ) );
        if( !program.has_value() )
        {
            return std::unexpected( std::move( program.error() ) );
        }
        auto seat = SessionSeat::open( display, layout );
        if( !seat.has_value() )
        {
            return std::unexpected( std::move( seat.error() ) );
        }

        const InterruptTrap            trap;
        SeatRunner<SessionSeat>        runner{ *seat };
        OutstandingHolds<SessionSeat>  holds{ runner };
        grab::kernel::sequence::Player player{ *program, runner };
        auto                           outcome  = grab::cli::drive( player );
        const auto                     released = holds.release();
        if( outcome.has_value() && released == grab::NeutralizationOutcome::Failed )
        {
            return grab::fail( grab::ErrorCode::ProviderFailed,
                               "the command left a button, key or pointer "
                               "capture down and it could not be released" );
        }
        return outcome;
    }

    int
    run_play_command( std::span<const std::string_view> args )
    {
        auto options = parse_play_options( args );
        if( !options.has_value() )
        {
            print_error( options.error().message );
            print_play_usage();
            return usageExitCode;
        }

        // Timed unconditionally, with two clock reads. `load` happens once per
        // process, and gating a number a human explicitly asked for behind a
        // compile level would make --trace a facility you have to rebuild for.
        //
        // The loader's own instrument is thread-local and accumulates across
        // calls by design, so the window has to be opened explicitly. Without
        // the reset a second `grab play` in one process would report the first
        // one's parse alongside its own.
        grab::kernel::sequence::reset_load_instrument();
        const auto started_loading = Clock::now();

        // THE PUBLIC LOADER, on purpose: grab::sequence::load_file is the
        // entry point embedders get, and this CLI running through it is what
        // keeps the two from drifting. It parses AND validates; the kernel
        // document behind the facade is reached through the in-repo unwrap
        // seam for the planning and tracing surfaces below.
        auto       loaded =
            grab::sequence::load_file( std::filesystem::path{ options->document } );
        if( !loaded.has_value() )
        {
            print_error( loaded.error().message );
            return runtimeExitCode;
        }
        const auto& program = grab::sequence::detail::unwrap( *loaded );

        const auto  pacing  = effective_pacing( program.pacing(), *options );
        auto        paced   = grab::cli::with_pacing( program, pacing );
        if( !paced.has_value() )
        {
            print_error( paced.error().message );
            return runtimeExitCode;
        }

        // The whole load: read, parse, validate, and rebuild under the
        // effective pacing. Measured as one span because that is what a caller
        // waits for; the phases inside it belong to the loader and arrive
        // through load_phases when it exposes them.
        RunTrace trace;
        trace.load = Clock::now() - started_loading;

        // Captured HERE, before planned() and unestimated_steps() run: those
        // record `ops.*` into the same thread-local instrument, and a phase
        // breakdown of the load that includes the cost of describing the load
        // is a breakdown of the wrong thing.
        trace.load_phases = grab::kernel::sequence::load_instrument();

        trace.sequence    = paced->name();
        trace.steps       = paced->steps().size();
        trace.planned     = grab::kernel::sequence::planned( *paced, pacing );
        trace.unestimated = grab::kernel::sequence::unestimated_steps( *paced );

        if( options->dry_run )
        {
            const auto text = dry_run_report( *paced, pacing );
            ( void )std::fwrite( text.data(), sizeof( char ), text.size(), stdout );
            if( options->trace )
            {
                // ran stays false, so the report prints the load timing and
                // omits the run section entirely rather than filling it with
                // zeroes from a run that never happened.
                const auto report = trace_report( trace );
                ( void )
                    std::fwrite( report.data(), sizeof( char ), report.size(), stdout );
            }
            return successExitCode;
        }

        auto seat = SessionSeat::open( nullptr, std::string_view{} );
        if( !seat.has_value() )
        {
            print_error( seat.error().message );
            return runtimeExitCode;
        }

        // Declared after the seat so it is DESTROYED FIRST: it borrows the
        // session and overlay the seat owns, and a presenter torn down after
        // its session is a use-after-free rather than a leak.
        VisualFeedback visuals;
        if( options->trail || options->feedback )
        {
            // Eagerly, before the first step runs. Opened lazily on the first
            // overlay step instead, the first moves of the run would produce no
            // trail at all -- and a document that draws nothing would produce
            // none ever.
            auto opened = seat->open_session();
            if( !opened.has_value() )
            {
                print_error( opened.error().message );
                return runtimeExitCode;
            }
            auto started = visuals.start( *seat->session(), *seat->surface(), *options );
            if( !started.has_value() )
            {
                print_error( started.error().message );
                return runtimeExitCode;
            }
        }

        // The trap is armed before the runner and disarmed after the holds are
        // lifted, so there is no window in which a signal can reach a run that
        // has already stopped tracking what it is holding.
        const InterruptTrap           trap;
        SeatRunner<SessionSeat>       runner{ *seat };
        OutstandingHolds<SessionSeat> holds{ runner };
        const int  code     = play_program( *paced,
                                            runner,
                                            *options,
                                            options->trace ? &trace : nullptr,
                                            [&visuals]
                                            {
                                           visuals.pump();
                                            } );
        const auto released = holds.release();

        // Explicit on the normal, failed AND signalled paths -- drive() turns a
        // signal into an ordinary unwind, so control reaches here for all
        // three. The destructor covers only the fourth, a throw.
        visuals.stop();

        // Printed on every exit path a run can reach, including a failed one:
        // a run that aborted at step 140 is exactly when a human wants to know
        // where the first 139 went.
        if( options->trace )
        {
            const auto report = trace_report( trace );
            ( void )std::fwrite( report.data(), sizeof( char ), report.size(), stdout );
        }

        log::nominal(
            [&paced, released, &options]( auto& event )
            {
                event.tag( log::tags::player )
                    .value( "played", paced->name() )
                    .value( "outstanding", neutralization_name( released ) )
                    .value( "trail", options->trail )
                    .value( "feedback", options->feedback );
            }
        );

        if( released == grab::NeutralizationOutcome::Failed )
        {
            print_error( "the document left a button, key or pointer capture down "
                         "and it could not be released" );
            return runtimeExitCode;
        }
        // A requested overlay that failed to draw is a failure, exactly as it is
        // for `grab trail` and `grab feedback` -- reported after the run's own
        // verdict, which is the more important one when both went wrong.
        if( visuals.error().has_value() )
        {
            print_error( visuals.error()->message );
            return code == successExitCode ? runtimeExitCode : code;
        }
        return code;
    }

}    // namespace grab::cli
