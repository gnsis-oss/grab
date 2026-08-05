#include "frontends/cli/common.hpp"
#include "frontends/cli/feedback_command.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "kernel/scheduling/reactor.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
// NOLINTNEXTLINE(modernize-deprecated-headers,misc-include-cleaner): POSIX signals.
#include <signal.h>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
// NOLINTNEXTLINE(misc-include-cleaner): EPOLL* constants are provided here.
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

namespace grab::cli
{
    namespace
    {

        constexpr std::string_view noClickFlag       = "--no-click";
        constexpr std::string_view noHoldFlag        = "--no-hold";
        constexpr std::string_view holdMsFlag        = "--hold-ms";
        constexpr std::string_view doubleClickMsFlag = "--double-click-ms";
        constexpr std::string_view pauseMsFlag       = "--pause-ms";
        constexpr std::string_view slopPxFlag        = "--slop-px";
        constexpr std::string_view rippleRadiusFlag  = "--ripple-radius";
        constexpr std::string_view rippleMsFlag      = "--ripple-ms";
        constexpr std::string_view barWidthFlag      = "--bar-width";
        constexpr std::string_view barHeightFlag     = "--bar-height";
        constexpr std::uint8_t     decimalDigitBase  = 10U;
        constexpr std::uint32_t    signalEvents =
            static_cast<std::uint32_t>( EPOLLIN | EPOLLERR | EPOLLHUP );
        constexpr int         posixSuccess        = 0;
        constexpr int         invalidFile         = -1;
        constexpr std::size_t firstArgument       = 0U;
        constexpr std::size_t optionValueStep     = 2U;
        constexpr std::size_t feedbackOptionCount = 10U;

        [[nodiscard]]
        constexpr bool
        is_value_option( std::string_view flag ) noexcept
        {
            return flag ==
                   holdMsFlag ||
                   flag ==
                   doubleClickMsFlag ||
                   flag ==
                   pauseMsFlag ||
                   flag ==
                   slopPxFlag ||
                   flag ==
                   rippleRadiusFlag ||
                   flag ==
                   rippleMsFlag ||
                   flag ==
                   barWidthFlag ||
                   flag == barHeightFlag;
        }

        [[nodiscard]]
        constexpr bool
        is_known_option( std::string_view flag ) noexcept
        {
            return flag == noClickFlag || flag == noHoldFlag || is_value_option( flag );
        }

        [[nodiscard]]
        constexpr bool
        is_duration_option( std::string_view flag ) noexcept
        {
            return flag ==
                   holdMsFlag ||
                   flag ==
                   doubleClickMsFlag ||
                   flag ==
                   pauseMsFlag ||
                   flag == rippleMsFlag;
        }

        struct CompletionState
        {
                std::mutex              mutex;
                std::condition_variable changed;
                bool                    complete{};
        };

        void
        complete( const std::shared_ptr<CompletionState>& state ) noexcept
        {
            {
                const std::scoped_lock lock{ state->mutex };
                state->complete = true;
            }
            state->changed.notify_all();
        }

        void
        wait_for_completion( CompletionState& state )
        {
            std::unique_lock lock{ state.mutex };
            state.changed.wait( lock,
                                [&state]
                                {
                                    return state.complete;
                                } );
        }

        [[nodiscard]]
        Error
        system_error( std::string_view operation,
                      int              error_number )
        {
            return Error{
                .code = ErrorCode::InternalFault,
                .message =
                    std::string{ operation }
                    +
                    ": " +
                    std::error_code{ error_number, std::generic_category() }
                    .message(),
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
        }

        class BlockedSignals final
        {
            public:

                [[nodiscard]]
                static Result<BlockedSignals>
                create()
                {
                    sigset_t signals{};    // NOLINT(misc-include-cleaner)
                    // NOLINTNEXTLINE(misc-include-cleaner): provided by POSIX signal.h.
                    if( ::sigemptyset( &signals ) !=
                        posixSuccess ||
                        // NOLINTNEXTLINE(misc-include-cleaner): provided by POSIX
                        // signal.h.
                        ::sigaddset( &signals, SIGINT ) !=
                        posixSuccess ||
                        // NOLINTNEXTLINE(misc-include-cleaner): provided by POSIX
                        // signal.h.
                        ::sigaddset( &signals, SIGTERM ) != posixSuccess )
                    {
                        return std::unexpected(
                            system_error( "configure feedback signals", errno )
                        );
                    }

                    sigset_t  old_mask{};    // NOLINT(misc-include-cleaner)
                    const int blocked =
                        ::pthread_sigmask( SIG_BLOCK, &signals, &old_mask );
                    if( blocked != posixSuccess )
                    {
                        return std::unexpected( system_error( "block feedback signals",
                                                              blocked ) );
                    }

                    const int descriptor =
                        ::signalfd( invalidFile, &signals, SFD_CLOEXEC | SFD_NONBLOCK );
                    if( descriptor == invalidFile )
                    {
                        const int open_error = errno;
                        static_cast<void>(
                            ::pthread_sigmask( SIG_SETMASK, &old_mask, nullptr )
                        );
                        return std::unexpected(
                            system_error( "open feedback signal descriptor", open_error )
                        );
                    }
                    return BlockedSignals{ descriptor, old_mask };
                }

                ~BlockedSignals()
                {
                    reset();
                }

                BlockedSignals( const BlockedSignals& ) = delete;
                BlockedSignals&
                operator=( const BlockedSignals& ) = delete;

                BlockedSignals( BlockedSignals&& other ) noexcept :
                    descriptor_{
                        std::exchange( other.descriptor_,
                                       invalidFile ),
                    },
                    old_mask_{ other.old_mask_ },
                    restore_mask_{
                        std::exchange( other.restore_mask_,
                                       false ),
                    }
                {
                }

                BlockedSignals&
                operator=( BlockedSignals&& other ) noexcept
                {
                    if( this != &other )
                    {
                        reset();
                        descriptor_   = std::exchange( other.descriptor_, invalidFile );
                        old_mask_     = other.old_mask_;
                        restore_mask_ = std::exchange( other.restore_mask_, false );
                    }
                    return *this;
                }

                [[nodiscard]]
                int
                descriptor() const noexcept
                {
                    return descriptor_;
                }

            private:

                BlockedSignals(
                    int      descriptor,
                    sigset_t old_mask    // NOLINT(misc-include-cleaner)
                ) noexcept :
                    descriptor_{ descriptor },
                    old_mask_{ old_mask },
                    restore_mask_{ true }
                {
                }

                void
                reset() noexcept
                {
                    if( descriptor_ != invalidFile )
                    {
                        static_cast<void>( ::close( descriptor_ ) );
                        descriptor_ = invalidFile;
                    }
                    if( restore_mask_ )
                    {
                        static_cast<void>(
                            ::pthread_sigmask( SIG_SETMASK, &old_mask_, nullptr )
                        );
                        restore_mask_ = false;
                    }
                }

                int      descriptor_{ invalidFile };
                sigset_t old_mask_{};    // NOLINT(misc-include-cleaner)
                bool     restore_mask_{};
        };

        [[nodiscard]]
        Result<std::chrono::milliseconds>
        parse_duration( std::string_view text,
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
                return fail( ErrorCode::InvalidArgument,
                             std::string{ flag } +
                                 " must be a nonnegative millisecond count" );
            }
            return std::chrono::milliseconds{ value };
        }

        [[nodiscard]]
        Result<double>
        parse_number( std::string_view text,
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
                return fail( ErrorCode::InvalidArgument,
                             std::string{ flag } +
                                 " must be a finite nonnegative number" );
            }
            return value;
        }

        [[nodiscard]]
        Result<void>
        apply_duration_option( std::string_view   flag,
                               std::string_view   value,
                               GestureThresholds& thresholds,
                               RippleStyle&       click )
        {
            auto parsed = parse_duration( value, flag );
            if( !parsed.has_value() )
            {
                return std::unexpected( std::move( parsed.error() ) );
            }
            if( flag == holdMsFlag )
            {
                thresholds.hold = *parsed;
            }
            else if( flag == doubleClickMsFlag )
            {
                thresholds.double_click = *parsed;
            }
            else if( flag == pauseMsFlag )
            {
                thresholds.pause = *parsed;
            }
            else
            {
                click.duration = *parsed;
            }
            return {};
        }

        [[nodiscard]]
        Result<void>
        apply_number_option( std::string_view   flag,
                             std::string_view   value,
                             GestureThresholds& thresholds,
                             RippleStyle&       click,
                             ProgressStyle&     hold )
        {
            auto parsed = parse_number( value, flag );
            if( !parsed.has_value() )
            {
                return std::unexpected( std::move( parsed.error() ) );
            }
            if( flag == slopPxFlag )
            {
                thresholds.slop_px = *parsed;
            }
            else if( flag == rippleRadiusFlag )
            {
                click.radius_px = *parsed;
            }
            else if( flag == barWidthFlag )
            {
                hold.width_px = *parsed;
            }
            else
            {
                hold.height_px = *parsed;
            }
            return {};
        }

        [[nodiscard]]
        Result<void>
        apply_value_option( std::string_view   flag,
                            std::string_view   value,
                            GestureThresholds& thresholds,
                            RippleStyle&       click,
                            ProgressStyle&     hold )
        {
            if( is_duration_option( flag ) )
            {
                return apply_duration_option( flag, value, thresholds, click );
            }
            return apply_number_option( flag, value, thresholds, click, hold );
        }

        [[nodiscard]]
        Result<std::vector<std::string_view>>
        argument_views( std::span<char* const> args )
        {
            std::vector<std::string_view> result;
            result.reserve( args.size() );
            for( const char* const argument : args )
            {
                if( argument == nullptr )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "feedback command argument is missing" );
                }
                result.emplace_back( argument );
            }
            return result;
        }

        [[nodiscard]]
        Result<void>
        reactor_barrier( Session& session )
        {
            auto completion = std::make_shared<std::promise<void>>();
            auto completed  = completion->get_future();
            auto posted     = session.post(
                [completion]
                {
                    completion->set_value();
                }
            );
            if( !posted.has_value() )
            {
                return std::unexpected( std::move( posted.error() ) );
            }
            completed.get();
            return {};
        }

        [[nodiscard]]
        Result<void>
        wait_until_signal( Session&              session,
                           const BlockedSignals& signals )
        {
            auto          state = std::make_shared<CompletionState>();
            std::uint64_t token{};
            try
            {
                token = session.reactor().add_fd(
                    signals.descriptor(),
                    signalEvents,
                    [state, descriptor = signals.descriptor()]( std::uint32_t events )
                    {
                        signalfd_siginfo information{};
                        const auto       bytes =
                            ::read( descriptor, &information, sizeof( information ) );
                        if( bytes >
                            0 ||
                            ( events &
                              static_cast<std::uint32_t>( EPOLLERR | EPOLLHUP ) ) != 0U )
                        {
                            complete( state );
                        }
                    }
                );
            }
            catch( const std::exception& error )
            {
                return fail( ErrorCode::InternalFault,
                             std::string{ "watch feedback signals: " } + error.what() );
            }
            catch( ... )
            {
                return fail( ErrorCode::InternalFault, "watch feedback signals failed" );
            }

            wait_for_completion( *state );
            session.reactor().remove_fd( token );
            return reactor_barrier( session );
        }

        [[nodiscard]]
        Result<void>
        execute_feedback( const CursorFeedbackConfig& config )
        {
            auto signals = BlockedSignals::create();
            if( !signals.has_value() )
            {
                return std::unexpected( std::move( signals.error() ) );
            }

            auto session = Session::open();
            if( !session.has_value() )
            {
                return std::unexpected( std::move( session.error() ) );
            }
            auto feedback = ( *session )->cursor_feedback( config );
            if( !feedback.has_value() )
            {
                return std::unexpected( std::move( feedback.error() ) );
            }

            auto waited = wait_until_signal( **session, *signals );
            if( !waited.has_value() )
            {
                return std::unexpected( std::move( waited.error() ) );
            }
            return feedback->status();
        }

        int
        report_parse_error( const Error& error )
        {
            print_error( error.message );
            return usageExitCode;
        }

        int
        report_runtime_result( Result<void> result )
        {
            if( result.has_value() )
            {
                return successExitCode;
            }
            print_error( result.error().message );
            return runtimeExitCode;
        }

    }    // namespace

    Result<CursorFeedbackConfig>
    parse_feedback_options( std::span<const std::string_view> args )
    {
        RippleStyle                   click{};
        ProgressStyle                 hold{};
        GestureThresholds             thresholds{};
        bool                          click_enabled{ true };
        bool                          hold_enabled{ true };
        std::vector<std::string_view> seen;
        seen.reserve( feedbackOptionCount );

        for( std::size_t index = firstArgument; index < args.size(); )
        {
            const auto flag = args.subspan( index, 1U ).front();
            if( !is_known_option( flag ) ||
                std::ranges::find( seen, flag ) != seen.end() )
            {
                return fail( ErrorCode::InvalidArgument,
                             "unknown or repeated feedback option: " +
                                 std::string{ flag } );
            }
            seen.push_back( flag );

            if( flag == noClickFlag )
            {
                click_enabled = false;
                ++index;
                continue;
            }
            if( flag == noHoldFlag )
            {
                hold_enabled = false;
                ++index;
                continue;
            }
            if( index + 1U >= args.size() )
            {
                return fail( ErrorCode::InvalidArgument,
                             std::string{ flag } + " requires a value" );
            }

            const auto value = args.subspan( index, optionValueStep ).back();
            auto applied = apply_value_option( flag, value, thresholds, click, hold );
            if( !applied.has_value() )
            {
                return std::unexpected( std::move( applied.error() ) );
            }
            index += optionValueStep;
        }

        return CursorFeedbackConfig{
            .click = click_enabled ? std::optional<RippleStyle>{ click } : std::nullopt,
            .hold  = hold_enabled ? std::optional<ProgressStyle>{ hold } : std::nullopt,
            .thresholds = thresholds,
        };
    }

    int
    run_feedback_command( std::span<char* const> args )
    {
        auto views = argument_views( args );
        if( !views.has_value() )
        {
            return report_parse_error( views.error() );
        }
        auto parsed = parse_feedback_options( *views );
        if( !parsed.has_value() )
        {
            return report_parse_error( parsed.error() );
        }
        return report_runtime_result( execute_feedback( *parsed ) );
    }

}    // namespace grab::cli
