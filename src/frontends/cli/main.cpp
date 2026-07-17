#include "client/client.hpp"
#include "client/loopback_transport.hpp"
#include "client/unix_socket_transport.hpp"
#include "codec/png.hpp"
#include "drivers/desktop/x11/window_match.hpp"
#include "drivers/desktop/x11/window_tracker.hpp"
#include "drivers/desktop/x11/workflow.hpp"
#include "frontends/cli/common.hpp"
#include "frontends/cli/input_command.hpp"
#include "frontends/cli/session_command.hpp"
#include "frontends/grpc/daemon.hpp"
#include "grab/capture.hpp"
#include "grab/command_descriptor.hpp"
#include "grab/event.hpp"
#include "grab/geometry/rectangle.hpp"
#include "grab/image.hpp"
#include "grab/input.hpp"
#include "grab/interaction.hpp"
#include "grab/locator.hpp"
#include "grab/pointer_button.hpp"
#include "grab/query.hpp"
#include "grab/result.hpp"
#include "grab/role.hpp"
#include "grab/screen.hpp"
#include "grab/session.hpp"
#include "grab/trace.hpp"
#include "grab/ui.hpp"
#include "image/compare.hpp"
#include "kernel/events/event_bus.hpp"
#include "kernel/routing/doctor.hpp"
#include "kernel/routing/prober.hpp"
#include "kernel/routing/registry.hpp"
#include "kernel/scheduling/reactor.hpp"
#include "notify/notifier.hpp"

#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
// NOLINTNEXTLINE(modernize-deprecated-headers,misc-include-cleaner): POSIX sigwait API.
#include <signal.h>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace
{

    constexpr int           usageError              = grab::cli::usageExitCode;
    constexpr int           runtimeError            = grab::cli::runtimeExitCode;
    constexpr int           signalSuccess           = grab::cli::successExitCode;
    constexpr char          coordinateSeparator     = ',';
    constexpr char          dimensionSeparator      = 'x';
    constexpr char          upperDimensionSeparator = 'X';
    constexpr std::uint64_t noDiffPixels            = 0U;
    constexpr std::time_t   signalPollSeconds       = 0;
    constexpr auto signalPollNanoseconds = decltype( timespec{}.tv_nsec ){ 100'000'000 };

    enum class CaptureTarget : std::uint8_t
    {
        None,
        Window,
        Output,
        Display,
        Region,
    };

    using CaptureRegion = grab::geometry::Rectangle;

    struct CaptureOptions
    {
            CaptureTarget         target = CaptureTarget::None;
            std::string           wm_class;
            std::string           output_name;
            std::string           endpoint;
            CaptureRegion         region;
            std::filesystem::path output;
            bool                  has_output = false;
    };

    struct BatchOptions
    {
            std::vector<grab::screen::BatchItem> items;
    };

    struct CompareOptions
    {
            std::filesystem::path first;
            std::filesystem::path second;
            bool                  notify     = false;
            bool                  has_first  = false;
            bool                  has_second = false;
    };

    struct WatchOptions
    {
            std::string           wm_class;
            std::string           endpoint;
            std::filesystem::path output;
            bool                  has_window = false;
            bool                  has_output = false;
    };

    struct TypeOptions
    {
            std::string text;
            std::string locator;
            std::string endpoint;
            const char* display = nullptr;
            std::string layout;
            bool        has_text    = false;
            bool        has_locator = false;
    };

    struct ClickOptions
    {
            grab::input::Point at;
            std::string        locator;
            std::string        endpoint;
            const char*        display      = nullptr;
            std::uint8_t       button       = grab::input::primaryButton;
            bool               has_at       = false;
            bool               has_locator  = false;
            bool               has_endpoint = false;
    };

    struct DragOptions
    {
            grab::input::Point from;
            grab::input::Point to;
            const char*        display  = nullptr;
            bool               has_from = false;
            bool               has_to   = false;
    };

    [[nodiscard]]
    grab::Result<grab::client::Client>
    make_verb_client( const std::string& endpoint )
    {
        if( !endpoint.empty() )
        {
            return grab::client::Client{
                std::make_unique<grab::client::UnixSocketTransport>( endpoint )
            };
        }
        auto session = grab::Session::open( grab::SessionOptions{} );
        if( !session.has_value() )
        {
            return std::unexpected( std::move( session.error() ) );
        }
        return grab::client::Client{
            std::make_unique<grab::client::LoopbackTransport>( std::move( *session ) )
        };
    }

    struct SignalState
    {
            std::mutex              mutex;
            std::condition_variable signalled;
            bool                    ready = false;
    };

    int
    run_doctor_command( bool as_json )
    {
        const auto facts    = grab::core::real_system_facts();
        const auto env      = grab::core::probe_environment( facts );
        const auto registry = grab::core::builtin_registry();
        const auto report   = grab::core::run_doctor( registry, env );
        const auto output =
            as_json ? grab::core::to_json( report ) : grab::core::to_text( report );
        ( void )std::fputs( output.c_str(), stdout );
        if( as_json )
        {
            ( void )std::fputc( '\n', stdout );
        }
        return grab::core::doctor_exit_code( report );
    }

    void
    print_usage()
    {
        ( void )std::fputs( "usage: grab doctor [--json]\n"
                            "       grab daemon [--endpoint ENDPOINT] [--store DIR]\n",
                            stderr );
        ( void )std::fputs( "       grab type --text TEXT [--display D] [--layout L] "
                            "[--locator LOCATOR [--endpoint ENDPOINT]]\n"
                            "       grab click --at X,Y [--button N] [--display D]\n"
                            "       grab click --locator LOCATOR [--endpoint ENDPOINT]\n"
                            "       grab drag --from X,Y --to X,Y [--display D]\n"
                            "       grab key --window APP --keysym NAME [--display D] "
                            "[--layout L]\n",
                            stderr );
        ( void )std::fputs( "       grab drag-curve --window APP --src X,Y --dst X,Y "
                            "[--display D]\n",
                            stderr );
        ( void )std::fputs( "       grab capture --window WMCLASS --out FILE.png\n"
                            "       grab capture --output NAME --out FILE.png "
                            "[--endpoint ENDPOINT]\n"
                            "       grab capture --display --out FILE.png\n"
                            "       grab capture --region X,Y,WxH --out FILE.png\n",
                            stderr );
        ( void )std::fputs( "       grab batch --window WMCLASS --out FILE.png [...]\n"
                            "       grab compare A.png B.png [--notify]\n"
                            "       grab watch --window WMCLASS --out FILE.png\n",
                            stderr );
    }

    using Printer = int ( * )( std::FILE*,
                               const char*,
                               ... );

    void
    print_fatal( const char* detail )
    {
        const Printer print = std::fprintf;
        ( void )print( stderr, "grab: fatal: %s\n", detail );
    }

    using grab::cli::print_error;

    [[nodiscard]]
    grab::Result<std::int16_t>
    parse_int16( std::string_view text,
                 std::string_view name )
    {
        std::int32_t      value  = 0;
        const auto* const begin  = text.begin();
        const auto* const end    = text.end();
        const auto        parsed = std::from_chars( begin, end, value );
        if( text.empty() ||
            parsed.ec !=
            std::errc{} ||
            parsed.ptr !=
            end ||
            value <
            std::numeric_limits<std::int16_t>::min() ||
            value > std::numeric_limits<std::int16_t>::max() )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               std::string{ name } + " must be an int16 value" );
        }

        return static_cast<std::int16_t>( value );
    }

    [[nodiscard]]
    grab::Result<std::uint16_t>
    parse_positive_uint16( std::string_view text,
                           std::string_view name )
    {
        std::uint32_t     value  = 0U;
        const auto* const begin  = text.begin();
        const auto* const end    = text.end();
        const auto        parsed = std::from_chars( begin, end, value );
        if( text.empty() ||
            parsed.ec !=
            std::errc{} ||
            parsed.ptr !=
            end ||
            value ==
            0U ||
            value > std::numeric_limits<std::uint16_t>::max() )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               std::string{ name } + " must be in range 1..65535" );
        }

        return static_cast<std::uint16_t>( value );
    }

    [[nodiscard]]
    grab::Result<std::uint8_t>
    parse_button( std::string_view text )
    {
        std::uint32_t     value  = 0U;
        const auto* const begin  = text.begin();
        const auto* const end    = text.end();
        const auto        parsed = std::from_chars( begin, end, value );
        if( text.empty() ||
            parsed.ec !=
            std::errc{} ||
            parsed.ptr !=
            end ||
            value ==
            0U ||
            value > std::numeric_limits<std::uint8_t>::max() )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "button must be in range 1..255" );
        }

        return static_cast<std::uint8_t>( value );
    }

    [[nodiscard]]
    grab::Result<grab::input::Point>
    parse_point( std::string_view text,
                 std::string_view option )
    {
        const std::size_t separator = text.find( coordinateSeparator );
        if( separator == std::string_view::npos )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               std::string{ option } + " must be X,Y" );
        }

        auto x =
            parse_int16( text.substr( 0U, separator ), std::string{ option } + " x" );
        if( !x.has_value() )
        {
            return std::unexpected( std::move( x.error() ) );
        }

        auto y =
            parse_int16( text.substr( separator + 1U ), std::string{ option } + " y" );
        if( !y.has_value() )
        {
            return std::unexpected( std::move( y.error() ) );
        }

        return grab::input::Point{
            .x = *x,
            .y = *y,
        };
    }

    [[nodiscard]]
    std::size_t
    find_dimension_separator( std::string_view text,
                              std::size_t      offset ) noexcept
    {
        const std::size_t lower = text.find( dimensionSeparator, offset );
        const std::size_t upper = text.find( upperDimensionSeparator, offset );
        if( lower == std::string_view::npos )
        {
            return upper;
        }
        if( upper == std::string_view::npos )
        {
            return lower;
        }
        if( lower < upper )
        {
            return lower;
        }
        return upper;
    }

    [[nodiscard]]
    grab::Result<CaptureRegion>
    parse_capture_region( std::string_view text )
    {
        const std::size_t first_separator = text.find( coordinateSeparator );
        if( first_separator == std::string_view::npos )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "--region must be X,Y,WxH" );
        }

        const std::size_t second_separator =
            text.find( coordinateSeparator, first_separator + 1U );
        if( second_separator == std::string_view::npos )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "--region must be X,Y,WxH" );
        }

        const std::size_t dimension_separator =
            find_dimension_separator( text, second_separator + 1U );
        if( dimension_separator == std::string_view::npos )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "--region must be X,Y,WxH" );
        }

        auto x = parse_int16( text.substr( 0U, first_separator ), "--region x" );
        if( !x.has_value() )
        {
            return std::unexpected( std::move( x.error() ) );
        }

        auto y = parse_int16( text.substr( first_separator + 1U,
                                           second_separator - first_separator - 1U ),
                              "--region y" );
        if( !y.has_value() )
        {
            return std::unexpected( std::move( y.error() ) );
        }

        auto width = parse_positive_uint16(
            text.substr( second_separator + 1U,
                         dimension_separator - second_separator - 1U ),
            "--region width"
        );
        if( !width.has_value() )
        {
            return std::unexpected( std::move( width.error() ) );
        }

        auto height = parse_positive_uint16( text.substr( dimension_separator + 1U ),
                                             "--region height" );
        if( !height.has_value() )
        {
            return std::unexpected( std::move( height.error() ) );
        }

        return CaptureRegion{
            .x      = *x,
            .y      = *y,
            .width  = *width,
            .height = *height,
        };
    }

    void
    mark_signalled( SignalState& state ) noexcept
    {
        try
        {
            {
                const std::scoped_lock lock( state.mutex );
                state.ready = true;
            }
            state.signalled.notify_one();
        }
        catch( ... )
        {
            return;
        }
    }

    [[nodiscard]]
    bool
    configure_daemon_signal_mask(
        sigset_t& signals    // NOLINT(misc-include-cleaner)
    ) noexcept
    {
        // NOLINTNEXTLINE(misc-include-cleaner): provided by POSIX <signal.h>.
        if( ::sigemptyset( &signals ) != signalSuccess )
        {
            return false;
        }
        // NOLINTNEXTLINE(misc-include-cleaner): provided by POSIX <signal.h>.
        if( ::sigaddset( &signals, SIGINT ) != signalSuccess )
        {
            return false;
        }
        // NOLINTNEXTLINE(misc-include-cleaner): provided by POSIX <signal.h>.
        if( ::sigaddset( &signals, SIGTERM ) != signalSuccess )
        {
            return false;
        }
        // NOLINTNEXTLINE(misc-include-cleaner): provided by POSIX <signal.h>.
        return ::pthread_sigmask( SIG_BLOCK, &signals, nullptr ) == signalSuccess;
    }

    void
    restore_daemon_signal_mask(
        const sigset_t& signals    // NOLINT(misc-include-cleaner)
    ) noexcept
    {
        // NOLINTNEXTLINE(misc-include-cleaner): provided by POSIX <signal.h>.
        const int result = ::pthread_sigmask( SIG_UNBLOCK, &signals, nullptr );
        static_cast<void>( result );
    }

    void
    wait_for_daemon_signal( sigset_t     signals,    // NOLINT(misc-include-cleaner)
                            SignalState& state ) noexcept
    {
        int       received = 0;
        // NOLINTNEXTLINE(misc-include-cleaner): provided by POSIX <signal.h>.
        const int wait_result = ::sigwait( &signals, &received );
        static_cast<void>( wait_result );
        static_cast<void>( received );
        mark_signalled( state );
    }

    void
    wait_until_signalled( SignalState& state )
    {
        std::unique_lock lock( state.mutex );
        state.signalled.wait( lock,
                              [&]
                              {
                                  return state.ready;
                              } );
    }

    [[nodiscard]]
    bool
    watch_signal_pending(
        const sigset_t& signals    // NOLINT(misc-include-cleaner)
    ) noexcept
    {
        const timespec timeout{
            .tv_sec  = signalPollSeconds,
            .tv_nsec = signalPollNanoseconds,
        };
        const int received = ::sigtimedwait( &signals, nullptr, &timeout );
        return received == SIGINT || received == SIGTERM;
    }

    [[nodiscard]]
    bool
    parse_daemon_options( std::span<char*>              args,
                          grab::service::DaemonOptions& options )
    {
        constexpr std::string_view endpointFlag{ "--endpoint" };
        constexpr std::string_view storeFlag{ "--store" };

        auto                       current = args.begin();
        while( current != args.end() )
        {
            const std::string_view arg = *current;
            ++current;
            if( arg == endpointFlag )
            {
                if( current == args.end() )
                {
                    return false;
                }
                options.endpoint = *current;
                ++current;
                continue;
            }

            if( arg == storeFlag )
            {
                if( current == args.end() )
                {
                    return false;
                }
                options.store_dir = std::filesystem::path{ *current };
                ++current;
                continue;
            }

            return false;
        }

        return true;
    }

    [[nodiscard]]
    grab::Result<TypeOptions>
    parse_type_options( std::span<char*> args )
    {
        constexpr std::string_view textFlag{ "--text" };
        constexpr std::string_view displayFlag{ "--display" };
        constexpr std::string_view layoutFlag{ "--layout" };
        constexpr std::string_view locatorFlag{ "--locator" };
        constexpr std::string_view endpointFlag{ "--endpoint" };

        TypeOptions                options;
        bool                       has_layout   = false;
        bool                       has_endpoint = false;
        auto                       current      = args.begin();
        while( current != args.end() )
        {
            const std::string_view arg = *current;
            ++current;
            if( arg == textFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--text requires a value" );
                }
                options.text     = *current;
                options.has_text = true;
                ++current;
                continue;
            }

            if( arg == displayFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--display requires a value" );
                }
                options.display = *current;
                ++current;
                continue;
            }

            if( arg == layoutFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--layout requires a value" );
                }
                options.layout = *current;
                has_layout     = true;
                ++current;
                continue;
            }

            if( arg == locatorFlag )
            {
                if( options.has_locator )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "type accepts one --locator" );
                }
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--locator requires a value" );
                }
                options.locator     = *current;
                options.has_locator = true;
                ++current;
                continue;
            }

            if( arg == endpointFlag )
            {
                if( has_endpoint )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "type accepts one --endpoint" );
                }
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--endpoint requires a value" );
                }
                options.endpoint = *current;
                has_endpoint     = true;
                ++current;
                continue;
            }

            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "unknown option for type: " + std::string{ arg } );
        }

        if( !options.has_text )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "type requires --text" );
        }
        if( options.has_locator && ( options.display != nullptr || has_layout ) )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "--display/--layout are not supported with --locator" );
        }
        if( has_endpoint && !options.has_locator )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "--endpoint requires --locator" );
        }
        return options;
    }

    [[nodiscard]]
    grab::Result<ClickOptions>
    parse_click_options( std::span<char*> args )
    {
        constexpr std::string_view atFlag{ "--at" };
        constexpr std::string_view buttonFlag{ "--button" };
        constexpr std::string_view displayFlag{ "--display" };
        constexpr std::string_view locatorFlag{ "--locator" };
        constexpr std::string_view endpointFlag{ "--endpoint" };

        ClickOptions               options;
        bool                       has_button = false;
        auto                       current    = args.begin();
        while( current != args.end() )
        {
            const std::string_view arg = *current;
            ++current;
            if( arg == atFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--at requires a value" );
                }
                auto point = parse_point( *current, "--at" );
                if( !point.has_value() )
                {
                    return std::unexpected( std::move( point.error() ) );
                }
                options.at     = *point;
                options.has_at = true;
                ++current;
                continue;
            }

            if( arg == buttonFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--button requires a value" );
                }
                auto button = parse_button( *current );
                if( !button.has_value() )
                {
                    return std::unexpected( std::move( button.error() ) );
                }
                options.button = *button;
                has_button     = true;
                ++current;
                continue;
            }

            if( arg == displayFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--display requires a value" );
                }
                options.display = *current;
                ++current;
                continue;
            }

            if( arg == locatorFlag )
            {
                if( options.has_locator )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "click accepts one --locator" );
                }
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--locator requires a value" );
                }
                options.locator     = *current;
                options.has_locator = true;
                ++current;
                continue;
            }

            if( arg == endpointFlag )
            {
                if( options.has_endpoint )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "click accepts one --endpoint" );
                }
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--endpoint requires a value" );
                }
                options.endpoint     = *current;
                options.has_endpoint = true;
                ++current;
                continue;
            }

            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "unknown option for click: " + std::string{ arg } );
        }

        if( options.has_at == options.has_locator )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "click requires exactly one of --at or --locator" );
        }
        if( !options.has_at && ( has_button || options.display != nullptr ) )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "--button/--display require --at" );
        }
        if( options.has_endpoint && !options.has_locator )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "--endpoint requires --locator" );
        }
        return options;
    }

    [[nodiscard]]
    grab::Result<DragOptions>
    parse_drag_options( std::span<char*> args )
    {
        constexpr std::string_view fromFlag{ "--from" };
        constexpr std::string_view toFlag{ "--to" };
        constexpr std::string_view displayFlag{ "--display" };

        DragOptions                options;
        auto                       current = args.begin();
        while( current != args.end() )
        {
            const std::string_view arg = *current;
            ++current;
            if( arg == fromFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--from requires a value" );
                }
                auto point = parse_point( *current, "--from" );
                if( !point.has_value() )
                {
                    return std::unexpected( std::move( point.error() ) );
                }
                options.from     = *point;
                options.has_from = true;
                ++current;
                continue;
            }

            if( arg == toFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--to requires a value" );
                }
                auto point = parse_point( *current, "--to" );
                if( !point.has_value() )
                {
                    return std::unexpected( std::move( point.error() ) );
                }
                options.to     = *point;
                options.has_to = true;
                ++current;
                continue;
            }

            if( arg == displayFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--display requires a value" );
                }
                options.display = *current;
                ++current;
                continue;
            }

            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "unknown option for drag: " + std::string{ arg } );
        }

        if( !options.has_from || !options.has_to )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "drag requires --from and --to" );
        }
        return options;
    }

    [[nodiscard]]
    grab::Result<void>
    require_no_capture_target( const CaptureOptions& options )
    {
        if( options.target != CaptureTarget::None )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "capture requires exactly one target" );
        }
        return {};
    }

    [[nodiscard]]
    grab::Result<CaptureOptions>
    parse_capture_options( std::span<char*> args )
    {
        constexpr std::string_view windowFlag{ "--window" };
        constexpr std::string_view outputFlag{ "--output" };
        constexpr std::string_view displayFlag{ "--display" };
        constexpr std::string_view regionFlag{ "--region" };
        constexpr std::string_view outFlag{ "--out" };
        constexpr std::string_view endpointFlag{ "--endpoint" };

        CaptureOptions             options;
        bool                       has_endpoint = false;
        auto                       current      = args.begin();
        while( current != args.end() )
        {
            const std::string_view arg = *current;
            ++current;
            if( arg == windowFlag )
            {
                auto target = require_no_capture_target( options );
                if( !target.has_value() )
                {
                    return std::unexpected( std::move( target.error() ) );
                }
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--window requires a value" );
                }
                options.target   = CaptureTarget::Window;
                options.wm_class = *current;
                ++current;
                continue;
            }

            if( arg == outputFlag )
            {
                auto target = require_no_capture_target( options );
                if( !target.has_value() )
                {
                    return std::unexpected( std::move( target.error() ) );
                }
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--output requires a value" );
                }
                options.target      = CaptureTarget::Output;
                options.output_name = *current;
                ++current;
                continue;
            }

            if( arg == displayFlag )
            {
                auto target = require_no_capture_target( options );
                if( !target.has_value() )
                {
                    return std::unexpected( std::move( target.error() ) );
                }
                options.target = CaptureTarget::Display;
                continue;
            }

            if( arg == regionFlag )
            {
                auto target = require_no_capture_target( options );
                if( !target.has_value() )
                {
                    return std::unexpected( std::move( target.error() ) );
                }
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--region requires a value" );
                }
                auto region = parse_capture_region( *current );
                if( !region.has_value() )
                {
                    return std::unexpected( std::move( region.error() ) );
                }
                options.target = CaptureTarget::Region;
                options.region = *region;
                ++current;
                continue;
            }

            if( arg == outFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--out requires a value" );
                }
                options.output     = std::filesystem::path{ *current };
                options.has_output = true;
                ++current;
                continue;
            }

            if( arg == endpointFlag )
            {
                if( has_endpoint )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "capture accepts one --endpoint" );
                }
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--endpoint requires a value" );
                }
                options.endpoint = *current;
                has_endpoint     = true;
                ++current;
                continue;
            }

            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "unknown option for capture: " + std::string{ arg } );
        }

        if( options.target == CaptureTarget::None )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "capture requires --window, --output, --display, or "
                               "--region" );
        }
        if( !options.has_output || options.output.empty() )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "capture requires --out" );
        }
        if( has_endpoint &&
            options.target !=
            CaptureTarget::Output &&
            options.target != CaptureTarget::Window )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "--endpoint requires --output or --window" );
        }
        return options;
    }

    [[nodiscard]]
    grab::Result<BatchOptions>
    parse_batch_options( std::span<char*> args )
    {
        constexpr std::string_view windowFlag{ "--window" };
        constexpr std::string_view outFlag{ "--out" };

        BatchOptions               options;
        auto                       current = args.begin();
        while( current != args.end() )
        {
            const std::string_view arg = *current;
            ++current;
            if( arg != windowFlag )
            {
                return grab::fail(
                    grab::ErrorCode::InvalidArgument,
                    "batch expects repeated --window WMCLASS --out FILE"
                );
            }
            if( current == args.end() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "--window requires a value" );
            }
            std::vector<std::string> candidates{ std::string{ *current } };
            ++current;

            if( current == args.end() || std::string_view{ *current } != outFlag )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "--window must be followed by --out" );
            }
            ++current;
            if( current == args.end() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "--out requires a value" );
            }

            options.items.push_back( grab::screen::BatchItem{
                .wm_class_candidates = std::move( candidates ),
                .out_path            = std::string{ *current },
            } );
            ++current;
        }

        if( options.items.empty() )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "batch requires at least one --window/--out pair" );
        }
        return options;
    }

    [[nodiscard]]
    grab::Result<CompareOptions>
    parse_compare_options( std::span<char*> args )
    {
        constexpr std::string_view notifyFlag{ "--notify" };

        CompareOptions             options;
        for( const char* const raw_arg : args )
        {
            const std::string_view arg = raw_arg;
            if( arg == notifyFlag )
            {
                options.notify = true;
                continue;
            }

            if( !options.has_first )
            {
                options.first     = std::filesystem::path{ raw_arg };
                options.has_first = true;
                continue;
            }

            if( !options.has_second )
            {
                options.second     = std::filesystem::path{ raw_arg };
                options.has_second = true;
                continue;
            }

            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "compare requires A.png B.png [--notify]" );
        }

        if( !options.has_first || !options.has_second )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "compare requires A.png and B.png" );
        }
        return options;
    }

    [[nodiscard]]
    grab::Result<WatchOptions>
    parse_watch_options( std::span<char*> args )
    {
        constexpr std::string_view windowFlag{ "--window" };
        constexpr std::string_view outFlag{ "--out" };
        constexpr std::string_view endpointFlag{ "--endpoint" };

        WatchOptions               options;
        bool                       has_endpoint = false;
        auto                       current      = args.begin();
        while( current != args.end() )
        {
            const std::string_view arg = *current;
            ++current;
            if( arg == windowFlag )
            {
                if( options.has_window )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "watch accepts one --window" );
                }
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--window requires a value" );
                }
                options.wm_class   = *current;
                options.has_window = true;
                ++current;
                continue;
            }

            if( arg == outFlag )
            {
                if( options.has_output )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "watch accepts one --out" );
                }
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--out requires a value" );
                }
                options.output     = std::filesystem::path{ *current };
                options.has_output = true;
                ++current;
                continue;
            }

            if( arg == endpointFlag )
            {
                if( has_endpoint )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "watch accepts one --endpoint" );
                }
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--endpoint requires a value" );
                }
                options.endpoint = *current;
                has_endpoint     = true;
                ++current;
                continue;
            }

            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "unknown option for watch: " + std::string{ arg } );
        }

        if( !options.has_window || !options.has_output || options.output.empty() )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "watch requires --window and --out" );
        }
        return options;
    }

    [[nodiscard]]
    grab::Result<grab::Image>
    capture_image( grab::Screen&         screen,
                   const CaptureOptions& options )
    {
        switch( options.target )
        {
            case CaptureTarget::Display :
                return screen.display();
            case CaptureTarget::Region :
                return screen.region(
                    static_cast<std::int16_t>( options.region.x ),
                    static_cast<std::int16_t>( options.region.y ),
                    static_cast<std::uint16_t>( options.region.width ),
                    static_cast<std::uint16_t>( options.region.height )
                );
            case CaptureTarget::Window :
            case CaptureTarget::Output :
            case CaptureTarget::None :
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "capture target is missing" );
        }
        return grab::fail( grab::ErrorCode::InternalFault, "capture target is invalid" );
    }

    [[nodiscard]]
    grab::Result<void>
    write_png_file( const std::filesystem::path& path,
                    std::span<const std::byte>   bytes )
    {
        if( bytes.size() >
            static_cast<std::size_t>( std::numeric_limits<std::streamsize>::max() ) )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "PNG output is too large to write" );
        }

        std::vector<char> output_bytes;
        output_bytes.reserve( bytes.size() );
        for( const std::byte value : bytes )
        {
            output_bytes.push_back(
                static_cast<char>( std::to_integer<unsigned char>( value ) )
            );
        }

        std::ofstream stream{ path, std::ios::binary };
        if( !stream )
        {
            return grab::fail( grab::ErrorCode::ProviderFailed,
                               "failed to open output file: " + path.string() );
        }

        if( !output_bytes.empty() )
        {
            stream.write( output_bytes.data(),
                          static_cast<std::streamsize>( output_bytes.size() ) );
        }
        if( !stream )
        {
            return grab::fail( grab::ErrorCode::ProviderFailed,
                               "failed to write output file: " + path.string() );
        }

        return {};
    }

    [[nodiscard]]
    bool
    drain_watch_events( grab::client::SubscriptionStream& stream,
                        grab::client::Client&             capture_client,
                        const std::vector<std::string>&   candidates,
                        const std::filesystem::path&      output,
                        std::uint32_t&                    captured,
                        std::string&                      failure )
    {
        while( true )
        {
            auto next = stream.try_next();
            if( !next.has_value() )
            {
                failure = next.error().message;
                return false;
            }
            if( !next->has_value() )
            {
                return true;
            }
            const auto* event = std::get_if<grab::Event>( &**next );
            if( event == nullptr || event->kind != grab::EventKind::WindowTitleChanged )
            {
                continue;
            }
            const auto* change = std::get_if<grab::WindowChange>( &event->payload );
            if( change ==
                nullptr ||
                !grab::screen::wm_class_matches_any( change->app, candidates ) )
            {
                continue;
            }
            const auto locator = grab::sel::all(
                { grab::sel::role( grab::role::window ),
                  grab::sel::property( grab::property::window_class, change->app ) }
            );
            auto match = capture_client.resolve( locator );
            if( !match.has_value() )
            {
                failure = match.error().message;
                return false;
            }
            auto frame =
                capture_client.capture( grab::CaptureTarget{ std::move( *match ) },
                                        grab::CaptureOptions{} );
            if( !frame.has_value() )
            {
                failure = frame.error().message;
                return false;
            }
            auto encoded = grab::codec::encode_png( frame->image );
            if( !encoded.has_value() )
            {
                failure = encoded.error().message;
                return false;
            }
            auto written = write_png_file( output, *encoded );
            if( !written.has_value() )
            {
                failure = written.error().message;
                return false;
            }
            ++captured;
        }
    }

    void
    print_capture_success( const std::filesystem::path& path,
                           const grab::Image&           image )
    {
        const std::string output = path.string();
        const std::string width  = std::to_string( image.width );
        const std::string height = std::to_string( image.height );
        ( void )std::fputs( "wrote ", stdout );
        ( void )std::fwrite( output.data(), sizeof( char ), output.size(), stdout );
        ( void )std::fputs( " (", stdout );
        ( void )std::fwrite( width.data(), sizeof( char ), width.size(), stdout );
        ( void )std::fputc( dimensionSeparator, stdout );
        ( void )std::fwrite( height.data(), sizeof( char ), height.size(), stdout );
        ( void )std::fputs( ")\n", stdout );
    }

    void
    print_batch_success( const grab::screen::BatchResult& result )
    {
        const std::string captured = std::to_string( result.captured );
        const std::string missed   = std::to_string( result.misses.size() );
        ( void )std::fputs( "captured ", stdout );
        ( void )std::fwrite( captured.data(), sizeof( char ), captured.size(), stdout );
        ( void )std::fputs( " window(s), missed ", stdout );
        ( void )std::fwrite( missed.data(), sizeof( char ), missed.size(), stdout );
        ( void )std::fputs( "\n", stdout );

        for( const std::string& miss : result.misses )
        {
            ( void )std::fputs( "missed ", stdout );
            ( void )std::fwrite( miss.data(), sizeof( char ), miss.size(), stdout );
            ( void )std::fputc( '\n', stdout );
        }
    }

    void
    print_compare_success( const grab::image::DiffResult& diff )
    {
        const std::string match_ratio = std::to_string( diff.match_ratio );
        const std::string diff_pixels = std::to_string( diff.diff_pixels );
        ( void )std::fputs( "match_ratio=", stdout );
        ( void )std::fwrite( match_ratio.data(),
                             sizeof( char ),
                             match_ratio.size(),
                             stdout );
        ( void )std::fputs( " diff_pixels=", stdout );
        ( void )std::fwrite( diff_pixels.data(),
                             sizeof( char ),
                             diff_pixels.size(),
                             stdout );
        ( void )std::fputc( '\n', stdout );
    }

    void
    print_watch_success( std::uint32_t captured )
    {
        const std::string captured_text = std::to_string( captured );
        ( void )std::fputs( "captured ", stdout );
        ( void )std::fwrite( captured_text.data(),
                             sizeof( char ),
                             captured_text.size(),
                             stdout );
        ( void )std::fputs( " change(s)\n", stdout );
    }

    int
    run_type_command( std::span<char*> args )
    {
        auto options = parse_type_options( args );
        if( !options.has_value() )
        {
            print_error( options.error().message );
            print_usage();
            return usageError;
        }

        if( options->has_locator )
        {
            auto locator = grab::Locator::from_string( options->locator );
            if( !locator.has_value() )
            {
                print_error( locator.error().message );
                print_usage();
                return usageError;
            }
            auto client = make_verb_client( options->endpoint );
            if( !client.has_value() )
            {
                print_fatal( client.error().message.c_str() );
                return runtimeError;
            }
            auto receipt = client->perform(
                grab::TypeText{
                    .target = std::move( *locator ),
                    .text   = options->text,
                },
                grab::ActionOptions{}
            );
            if( !receipt.has_value() )
            {
                print_fatal( receipt.error().message.c_str() );
                return runtimeError;
            }
            return 0;
        }

        auto input = grab::Input::open( options->display, options->layout );
        if( !input.has_value() )
        {
            print_fatal( input.error().message.c_str() );
            return runtimeError;
        }

        auto result = input->type_text( options->text );
        if( !result.has_value() )
        {
            print_fatal( result.error().message.c_str() );
            return runtimeError;
        }
        return 0;
    }

    int
    run_click_command( std::span<char*> args )
    {
        auto options = parse_click_options( args );
        if( !options.has_value() )
        {
            print_error( options.error().message );
            print_usage();
            return usageError;
        }

        if( options->has_locator )
        {
            auto locator = grab::Locator::from_string( options->locator );
            if( !locator.has_value() )
            {
                print_error( locator.error().message );
                print_usage();
                return usageError;
            }
            auto client = make_verb_client( options->endpoint );
            if( !client.has_value() )
            {
                print_fatal( client.error().message.c_str() );
                return runtimeError;
            }
            auto receipt =
                client->perform( grab::Click{ .target = std::move( *locator ) },
                                 grab::ActionOptions{} );
            if( !receipt.has_value() )
            {
                print_fatal( receipt.error().message.c_str() );
                return runtimeError;
            }
            return 0;
        }

        auto input = grab::Input::open( options->display );
        if( !input.has_value() )
        {
            print_fatal( input.error().message.c_str() );
            return runtimeError;
        }

        auto result = input->click_at( static_cast<std::int16_t>( options->at.x ),
                                       static_cast<std::int16_t>( options->at.y ),
                                       options->button );
        if( !result.has_value() )
        {
            print_fatal( result.error().message.c_str() );
            return runtimeError;
        }
        return 0;
    }

    int
    run_drag_command( std::span<char*> args )
    {
        auto options = parse_drag_options( args );
        if( !options.has_value() )
        {
            print_error( options.error().message );
            print_usage();
            return usageError;
        }

        auto input = grab::Input::open( options->display );
        if( !input.has_value() )
        {
            print_fatal( input.error().message.c_str() );
            return runtimeError;
        }

        auto result = input->drag( options->from, options->to );
        if( !result.has_value() )
        {
            print_fatal( result.error().message.c_str() );
            return runtimeError;
        }
        return 0;
    }

    int
    run_capture_command( std::span<char*> args )
    {
        auto options = parse_capture_options( args );
        if( !options.has_value() )
        {
            print_error( options.error().message );
            print_usage();
            return usageError;
        }

        if( options->target ==
            CaptureTarget::Output ||
            options->target == CaptureTarget::Window )
        {
            auto client = make_verb_client( options->endpoint );
            if( !client.has_value() )
            {
                print_fatal( client.error().message.c_str() );
                return runtimeError;
            }
            grab::Result<grab::Frame> frame =
                grab::fail( grab::ErrorCode::InvalidArgument,
                            "capture target is missing" );
            if( options->target == CaptureTarget::Output )
            {
                frame = client->capture( grab::CaptureTarget{ options->output_name },
                                         grab::CaptureOptions{} );
            }
            else
            {
                const auto locator = grab::sel::all(
                    { grab::sel::role( grab::role::window ),
                      grab::sel::property( grab::property::window_class,
                                           options->wm_class ) }
                );
                auto match = client->resolve( locator );
                if( !match.has_value() )
                {
                    print_fatal( match.error().message.c_str() );
                    return runtimeError;
                }
                frame = client->capture( grab::CaptureTarget{ std::move( *match ) },
                                         grab::CaptureOptions{} );
            }
            if( !frame.has_value() )
            {
                print_fatal( frame.error().message.c_str() );
                return runtimeError;
            }
            auto encoded = grab::codec::encode_png( frame->image );
            if( !encoded.has_value() )
            {
                print_fatal( encoded.error().message.c_str() );
                return runtimeError;
            }
            auto written = write_png_file( options->output, *encoded );
            if( !written.has_value() )
            {
                print_fatal( written.error().message.c_str() );
                return runtimeError;
            }
            print_capture_success( options->output, frame->image );
            return 0;
        }

        auto screen = grab::Screen::open();
        if( !screen.has_value() )
        {
            print_fatal( screen.error().message.c_str() );
            return runtimeError;
        }

        auto image = capture_image( *screen, *options );
        if( !image.has_value() )
        {
            print_fatal( image.error().message.c_str() );
            return runtimeError;
        }

        auto encoded = grab::codec::encode_png( *image );
        if( !encoded.has_value() )
        {
            print_fatal( encoded.error().message.c_str() );
            return runtimeError;
        }

        auto written = write_png_file( options->output, *encoded );
        if( !written.has_value() )
        {
            print_fatal( written.error().message.c_str() );
            return runtimeError;
        }

        print_capture_success( options->output, *image );
        return 0;
    }

    int
    run_batch_command( std::span<char*> args )
    {
        auto options = parse_batch_options( args );
        if( !options.has_value() )
        {
            print_error( options.error().message );
            print_usage();
            return usageError;
        }

        auto screen = grab::Screen::open();
        if( !screen.has_value() )
        {
            print_fatal( screen.error().message.c_str() );
            return runtimeError;
        }

        auto result = grab::screen::batch_capture( *screen, options->items );
        if( !result.has_value() )
        {
            print_fatal( result.error().message.c_str() );
            return runtimeError;
        }

        print_batch_success( *result );
        return result->misses.empty() ? 0 : runtimeError;
    }

    int
    run_compare_command( std::span<char*> args )
    {
        auto options = parse_compare_options( args );
        if( !options.has_value() )
        {
            print_error( options.error().message );
            print_usage();
            return usageError;
        }

        std::optional<grab::notify::Notifier> notifier;
        if( options->notify )
        {
            auto opened = grab::notify::Notifier::open();
            if( !opened.has_value() )
            {
                print_fatal( opened.error().message.c_str() );
                return runtimeError;
            }
            notifier = std::move( *opened );
        }

        auto diff =
            grab::screen::compare_files( options->first.string(),
                                         options->second.string(),
                                         notifier.has_value() ? &*notifier : nullptr );
        if( !diff.has_value() )
        {
            print_fatal( diff.error().message.c_str() );
            return runtimeError;
        }

        print_compare_success( *diff );
        return diff->diff_pixels == noDiffPixels ? 0 : runtimeError;
    }

    int
    run_watch_command( std::span<char*> args )
    {
        auto options = parse_watch_options( args );
        if( !options.has_value() )
        {
            print_error( options.error().message );
            print_usage();
            return usageError;
        }
        if( !options->endpoint.empty() )
        {
            print_fatal( "watch over a remote endpoint is not yet supported" );
            return runtimeError;
        }

        sigset_t signals{};
        if( !configure_daemon_signal_mask( signals ) )
        {
            print_fatal( "failed to configure watch signal handling" );
            return runtimeError;
        }

        // Title-change detection: a local WindowTracker feeds a bus that the
        // client subscribes to over the loopback transport.
        grab::EventBus      bus;

        grab::core::Reactor reactor;
        std::thread         reactor_thread(
            [&reactor]
            {
                auto result = reactor.run();
                static_cast<void>( result );
            }
        );

        auto tracker =
            grab::drivers::desktop::x11::WindowTracker::start( nullptr, reactor, bus );
        if( !tracker.has_value() )
        {
            reactor.stop();
            reactor_thread.join();
            restore_daemon_signal_mask( signals );
            print_fatal( tracker.error().message.c_str() );
            return runtimeError;
        }

        grab::client::LoopbackTransport event_transport{ bus };
        grab::client::Client            event_client{ event_transport };
        auto stream         = event_client.subscribe( grab::EventFilter{
            .kinds      = { grab::EventKind::WindowTitleChanged },
            .categories = {},
        } );
        auto capture_client = make_verb_client( std::string{} );
        if( !stream.has_value() || !capture_client.has_value() )
        {
            const std::string failure = !stream.has_value()
                                          ? stream.error().message
                                          : capture_client.error().message;
            tracker->stop();
            reactor.stop();
            reactor_thread.join();
            restore_daemon_signal_mask( signals );
            print_fatal( failure.c_str() );
            return runtimeError;
        }

        ( void )std::fputs( "watching for title changes; press Ctrl-C to stop\n",
                            stdout );
        const std::vector<std::string> candidates =
            grab::screen::normalized_wm_class_candidates( { options->wm_class } );
        std::uint32_t captured = 0U;
        bool          failed   = false;
        std::string   failure;
        while( !watch_signal_pending( signals ) )
        {
            if( !drain_watch_events( **stream,
                                     *capture_client,
                                     candidates,
                                     options->output,
                                     captured,
                                     failure ) )
            {
                failed = true;
                break;
            }
        }
        if( !failed )
        {
            failed = !drain_watch_events( **stream,
                                          *capture_client,
                                          candidates,
                                          options->output,
                                          captured,
                                          failure );
        }

        tracker->stop();
        reactor.stop();
        reactor_thread.join();
        restore_daemon_signal_mask( signals );
        if( failed )
        {
            print_fatal( failure.c_str() );
            return runtimeError;
        }

        print_watch_success( captured );
        return 0;
    }

    int
    run_daemon_command( std::span<char*> args )
    {
        grab::service::DaemonOptions options;
        if( !parse_daemon_options( args, options ) )
        {
            print_usage();
            return usageError;
        }

        sigset_t signals{};
        if( !configure_daemon_signal_mask( signals ) )
        {
            print_fatal( "failed to configure daemon signal handling" );
            return runtimeError;
        }

        auto daemon = grab::service::Daemon::start( std::move( options ) );
        if( !daemon.has_value() )
        {
            restore_daemon_signal_mask( signals );
            print_fatal( daemon.error().message.c_str() );
            return runtimeError;
        }

        // Keep daemon-local producers and socket clients on the same Client
        // seam. Listing the descriptors is the existing protocol's health
        // operation and verifies both bindings without changing CLI output.
        grab::client::LoopbackTransport loopback_transport{ daemon->bus() };
        grab::client::Client            loopback_client{ loopback_transport };
        auto local_health = loopback_client.list_event_types();
        if( !local_health.has_value() )
        {
            restore_daemon_signal_mask( signals );
            print_fatal( local_health.error().message.c_str() );
            return runtimeError;
        }

        grab::client::Client daemon_client{
            std::make_unique<grab::client::UnixSocketTransport>( daemon->endpoint() )
        };
        auto remote_health = daemon_client.list_event_types();
        if( !remote_health.has_value() )
        {
            restore_daemon_signal_mask( signals );
            print_fatal( remote_health.error().message.c_str() );
            return runtimeError;
        }

        SignalState signal_state;
        std::thread signal_thread( wait_for_daemon_signal,
                                   signals,
                                   std::ref( signal_state ) );

        wait_until_signalled( signal_state );
        daemon->shutdown();
        if( signal_thread.joinable() )
        {
            signal_thread.join();
        }
        restore_daemon_signal_mask( signals );
        return 0;
    }

    int
    run( int    argc,
         char** argv )
    {
        if( argc < 2 )
        {
            print_usage();
            return usageError;
        }

        const std::span<char*> args( argv, static_cast<std::size_t>( argc ) );
        const auto             cli_args = args.subspan( 1 );
        const auto* const command = grab::cli::find_command_by_verb( cli_args.front() );
        constexpr std::string_view jsonFlag{ "--json" };

        if( command == nullptr )
        {
            print_usage();
            return usageError;
        }

        switch( command->kind )
        {
            case grab::CommandKind::Doctor :
                {
                    bool as_json = false;
                    for( const char* arg : cli_args.subspan( 1 ) )
                    {
                        if( std::string_view( arg ) == jsonFlag )
                        {
                            as_json = true;
                        }
                        else
                        {
                            print_usage();
                            return usageError;
                        }
                    }
                    return run_doctor_command( as_json );
                }
            case grab::CommandKind::Daemon :
                return run_daemon_command( cli_args.subspan( 1 ) );
            case grab::CommandKind::Type :
                return run_type_command( cli_args.subspan( 1 ) );
            case grab::CommandKind::Click :
                return run_click_command( cli_args.subspan( 1 ) );
            case grab::CommandKind::Drag :
                return run_drag_command( cli_args.subspan( 1 ) );
            case grab::CommandKind::DragCurve :
                return grab::cli::run_drag_curve_command( cli_args.subspan( 1 ) );
            case grab::CommandKind::Capture :
                return run_capture_command( cli_args.subspan( 1 ) );
            case grab::CommandKind::Batch :
                return run_batch_command( cli_args.subspan( 1 ) );
            case grab::CommandKind::Compare :
                return run_compare_command( cli_args.subspan( 1 ) );
            case grab::CommandKind::Watch :
                return run_watch_command( cli_args.subspan( 1 ) );
            case grab::CommandKind::Key :
                return grab::cli::run_key_command( cli_args.subspan( 1 ) );
            case grab::CommandKind::Session :
                {
                    std::vector<std::string_view> session_args;
                    session_args.reserve( cli_args.size() - 1U );
                    for( const char* arg : cli_args.subspan( 1 ) )
                    {
                        session_args.emplace_back( arg );
                    }
                    return grab::cli::run_session_command( session_args );
                }
            case grab::CommandKind::Count :
                break;
        }

        print_usage();
        return usageError;
    }

}    // namespace

int
main( int    argc,
      char** argv )
{
    try
    {
        return run( argc, argv );
    }
    catch( const std::exception& e )
    {
        print_fatal( e.what() );
        return 2;
    }
    catch( ... )
    {
        print_fatal( "unknown error" );
        return 2;
    }
}
