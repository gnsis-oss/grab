#include "cli/input_command.hpp"
#include "cli/session_command.hpp"
#include "codec/png.hpp"
#include "core/doctor.hpp"
#include "core/prober.hpp"
#include "core/registry.hpp"
#include "grab/geometry/rectangle.hpp"
#include "grab/image.hpp"
#include "grab/input.hpp"
#include "grab/result.hpp"
#include "grab/screen.hpp"
#include "image/compare.hpp"
#include "input/gestures.hpp"
#include "notify/notifier.hpp"
#include "screen/workflow.hpp"
#include "service/daemon.hpp"

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
#include <vector>

namespace
{

    constexpr int           kUsageError              = 2;
    constexpr int           kRuntimeError            = 1;
    constexpr int           kSignalSuccess           = 0;
    constexpr char          kCoordinateSeparator     = ',';
    constexpr char          kDimensionSeparator      = 'x';
    constexpr char          kUpperDimensionSeparator = 'X';
    constexpr std::uint8_t  kDefaultButton           = 1U;
    constexpr std::uint64_t kNoDiffPixels            = 0U;
    constexpr std::time_t   kSignalPollSeconds       = 0;
    constexpr auto          kSignalPollNanoseconds = decltype( timespec{}.tv_nsec ){ 0 };

    enum class CaptureTarget : std::uint8_t
    {
        none,
        window,
        display,
        region,
    };

    using CaptureRegion = grab::geometry::Rectangle;

    struct CaptureOptions
    {
            CaptureTarget         target = CaptureTarget::none;
            std::string           wm_class;
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
            std::filesystem::path output;
            bool                  has_window = false;
            bool                  has_output = false;
    };

    struct TypeOptions
    {
            std::string text;
            const char* display  = nullptr;
            std::string layout   = "us";
            bool        has_text = false;
    };

    struct ClickOptions
    {
            grab::input::Point at;
            const char*        display = nullptr;
            std::uint8_t       button  = kDefaultButton;
            bool               has_at  = false;
    };

    struct DragOptions
    {
            grab::input::Point from;
            grab::input::Point to;
            const char*        display  = nullptr;
            bool               has_from = false;
            bool               has_to   = false;
    };

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
        ( void )std::fputs( "       grab type --text TEXT [--display D] [--layout L]\n"
                            "       grab click --at X,Y [--button N] [--display D]\n"
                            "       grab drag --from X,Y --to X,Y [--display D]\n",
                            stderr );
        ( void )std::fputs( "       grab capture --window WMCLASS --out FILE.png\n"
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

    void
    print_error( std::string_view detail )
    {
        ( void )std::fputs( "grab: error: ", stderr );
        ( void )std::fwrite( detail.data(), sizeof( char ), detail.size(), stderr );
        ( void )std::fputc( '\n', stderr );
    }

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
            return grab::fail( grab::ErrorCode::invalid_argument,
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
            return grab::fail( grab::ErrorCode::invalid_argument,
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
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "button must be in range 1..255" );
        }

        return static_cast<std::uint8_t>( value );
    }

    [[nodiscard]]
    grab::Result<grab::input::Point>
    parse_point( std::string_view text,
                 std::string_view option )
    {
        const std::size_t separator = text.find( kCoordinateSeparator );
        if( separator == std::string_view::npos )
        {
            return grab::fail( grab::ErrorCode::invalid_argument,
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
        const std::size_t lower = text.find( kDimensionSeparator, offset );
        const std::size_t upper = text.find( kUpperDimensionSeparator, offset );
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
        const std::size_t first_separator = text.find( kCoordinateSeparator );
        if( first_separator == std::string_view::npos )
        {
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "--region must be X,Y,WxH" );
        }

        const std::size_t second_separator =
            text.find( kCoordinateSeparator, first_separator + 1U );
        if( second_separator == std::string_view::npos )
        {
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "--region must be X,Y,WxH" );
        }

        const std::size_t dimension_separator =
            find_dimension_separator( text, second_separator + 1U );
        if( dimension_separator == std::string_view::npos )
        {
            return grab::fail( grab::ErrorCode::invalid_argument,
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
        if( ::sigemptyset( &signals ) != kSignalSuccess )
        {
            return false;
        }
        // NOLINTNEXTLINE(misc-include-cleaner): provided by POSIX <signal.h>.
        if( ::sigaddset( &signals, SIGINT ) != kSignalSuccess )
        {
            return false;
        }
        // NOLINTNEXTLINE(misc-include-cleaner): provided by POSIX <signal.h>.
        if( ::sigaddset( &signals, SIGTERM ) != kSignalSuccess )
        {
            return false;
        }
        // NOLINTNEXTLINE(misc-include-cleaner): provided by POSIX <signal.h>.
        return ::pthread_sigmask( SIG_BLOCK, &signals, nullptr ) == kSignalSuccess;
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
            .tv_sec  = kSignalPollSeconds,
            .tv_nsec = kSignalPollNanoseconds,
        };
        const int received = ::sigtimedwait( &signals, nullptr, &timeout );
        return received == SIGINT || received == SIGTERM;
    }

    [[nodiscard]]
    bool
    parse_daemon_options( std::span<char*>              args,
                          grab::service::DaemonOptions& options )
    {
        constexpr std::string_view kEndpointFlag{ "--endpoint" };
        constexpr std::string_view kStoreFlag{ "--store" };

        auto                       current = args.begin();
        while( current != args.end() )
        {
            const std::string_view arg = *current;
            ++current;
            if( arg == kEndpointFlag )
            {
                if( current == args.end() )
                {
                    return false;
                }
                options.endpoint = *current;
                ++current;
                continue;
            }

            if( arg == kStoreFlag )
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
        constexpr std::string_view kTextFlag{ "--text" };
        constexpr std::string_view kDisplayFlag{ "--display" };
        constexpr std::string_view kLayoutFlag{ "--layout" };

        TypeOptions                options;
        auto                       current = args.begin();
        while( current != args.end() )
        {
            const std::string_view arg = *current;
            ++current;
            if( arg == kTextFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       "--text requires a value" );
                }
                options.text     = *current;
                options.has_text = true;
                ++current;
                continue;
            }

            if( arg == kDisplayFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       "--display requires a value" );
                }
                options.display = *current;
                ++current;
                continue;
            }

            if( arg == kLayoutFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       "--layout requires a value" );
                }
                options.layout = *current;
                ++current;
                continue;
            }

            return grab::fail( grab::ErrorCode::invalid_argument,
                               "unknown option for type: " + std::string{ arg } );
        }

        if( !options.has_text )
        {
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "type requires --text" );
        }
        return options;
    }

    [[nodiscard]]
    grab::Result<ClickOptions>
    parse_click_options( std::span<char*> args )
    {
        constexpr std::string_view kAtFlag{ "--at" };
        constexpr std::string_view kButtonFlag{ "--button" };
        constexpr std::string_view kDisplayFlag{ "--display" };

        ClickOptions               options;
        auto                       current = args.begin();
        while( current != args.end() )
        {
            const std::string_view arg = *current;
            ++current;
            if( arg == kAtFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
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

            if( arg == kButtonFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       "--button requires a value" );
                }
                auto button = parse_button( *current );
                if( !button.has_value() )
                {
                    return std::unexpected( std::move( button.error() ) );
                }
                options.button = *button;
                ++current;
                continue;
            }

            if( arg == kDisplayFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       "--display requires a value" );
                }
                options.display = *current;
                ++current;
                continue;
            }

            return grab::fail( grab::ErrorCode::invalid_argument,
                               "unknown option for click: " + std::string{ arg } );
        }

        if( !options.has_at )
        {
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "click requires --at" );
        }
        return options;
    }

    [[nodiscard]]
    grab::Result<DragOptions>
    parse_drag_options( std::span<char*> args )
    {
        constexpr std::string_view kFromFlag{ "--from" };
        constexpr std::string_view kToFlag{ "--to" };
        constexpr std::string_view kDisplayFlag{ "--display" };

        DragOptions                options;
        auto                       current = args.begin();
        while( current != args.end() )
        {
            const std::string_view arg = *current;
            ++current;
            if( arg == kFromFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
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

            if( arg == kToFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
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

            if( arg == kDisplayFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       "--display requires a value" );
                }
                options.display = *current;
                ++current;
                continue;
            }

            return grab::fail( grab::ErrorCode::invalid_argument,
                               "unknown option for drag: " + std::string{ arg } );
        }

        if( !options.has_from || !options.has_to )
        {
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "drag requires --from and --to" );
        }
        return options;
    }

    [[nodiscard]]
    grab::Result<void>
    require_no_capture_target( const CaptureOptions& options )
    {
        if( options.target != CaptureTarget::none )
        {
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "capture requires exactly one target" );
        }
        return {};
    }

    [[nodiscard]]
    grab::Result<CaptureOptions>
    parse_capture_options( std::span<char*> args )
    {
        constexpr std::string_view kWindowFlag{ "--window" };
        constexpr std::string_view kDisplayFlag{ "--display" };
        constexpr std::string_view kRegionFlag{ "--region" };
        constexpr std::string_view kOutFlag{ "--out" };

        CaptureOptions             options;
        auto                       current = args.begin();
        while( current != args.end() )
        {
            const std::string_view arg = *current;
            ++current;
            if( arg == kWindowFlag )
            {
                auto target = require_no_capture_target( options );
                if( !target.has_value() )
                {
                    return std::unexpected( std::move( target.error() ) );
                }
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       "--window requires a value" );
                }
                options.target   = CaptureTarget::window;
                options.wm_class = *current;
                ++current;
                continue;
            }

            if( arg == kDisplayFlag )
            {
                auto target = require_no_capture_target( options );
                if( !target.has_value() )
                {
                    return std::unexpected( std::move( target.error() ) );
                }
                options.target = CaptureTarget::display;
                continue;
            }

            if( arg == kRegionFlag )
            {
                auto target = require_no_capture_target( options );
                if( !target.has_value() )
                {
                    return std::unexpected( std::move( target.error() ) );
                }
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       "--region requires a value" );
                }
                auto region = parse_capture_region( *current );
                if( !region.has_value() )
                {
                    return std::unexpected( std::move( region.error() ) );
                }
                options.target = CaptureTarget::region;
                options.region = *region;
                ++current;
                continue;
            }

            if( arg == kOutFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       "--out requires a value" );
                }
                options.output     = std::filesystem::path{ *current };
                options.has_output = true;
                ++current;
                continue;
            }

            return grab::fail( grab::ErrorCode::invalid_argument,
                               "unknown option for capture: " + std::string{ arg } );
        }

        if( options.target == CaptureTarget::none )
        {
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "capture requires --window, --display, or --region" );
        }
        if( !options.has_output || options.output.empty() )
        {
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "capture requires --out" );
        }
        return options;
    }

    [[nodiscard]]
    grab::Result<BatchOptions>
    parse_batch_options( std::span<char*> args )
    {
        constexpr std::string_view kWindowFlag{ "--window" };
        constexpr std::string_view kOutFlag{ "--out" };

        BatchOptions               options;
        auto                       current = args.begin();
        while( current != args.end() )
        {
            const std::string_view arg = *current;
            ++current;
            if( arg != kWindowFlag )
            {
                return grab::fail(
                    grab::ErrorCode::invalid_argument,
                    "batch expects repeated --window WMCLASS --out FILE"
                );
            }
            if( current == args.end() )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "--window requires a value" );
            }
            std::vector<std::string> candidates{ std::string{ *current } };
            ++current;

            if( current == args.end() || std::string_view{ *current } != kOutFlag )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "--window must be followed by --out" );
            }
            ++current;
            if( current == args.end() )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
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
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "batch requires at least one --window/--out pair" );
        }
        return options;
    }

    [[nodiscard]]
    grab::Result<CompareOptions>
    parse_compare_options( std::span<char*> args )
    {
        constexpr std::string_view kNotifyFlag{ "--notify" };

        CompareOptions             options;
        for( const char* const raw_arg : args )
        {
            const std::string_view arg = raw_arg;
            if( arg == kNotifyFlag )
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

            return grab::fail( grab::ErrorCode::invalid_argument,
                               "compare requires A.png B.png [--notify]" );
        }

        if( !options.has_first || !options.has_second )
        {
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "compare requires A.png and B.png" );
        }
        return options;
    }

    [[nodiscard]]
    grab::Result<WatchOptions>
    parse_watch_options( std::span<char*> args )
    {
        constexpr std::string_view kWindowFlag{ "--window" };
        constexpr std::string_view kOutFlag{ "--out" };

        WatchOptions               options;
        auto                       current = args.begin();
        while( current != args.end() )
        {
            const std::string_view arg = *current;
            ++current;
            if( arg == kWindowFlag )
            {
                if( options.has_window )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       "watch accepts one --window" );
                }
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       "--window requires a value" );
                }
                options.wm_class   = *current;
                options.has_window = true;
                ++current;
                continue;
            }

            if( arg == kOutFlag )
            {
                if( options.has_output )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       "watch accepts one --out" );
                }
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       "--out requires a value" );
                }
                options.output     = std::filesystem::path{ *current };
                options.has_output = true;
                ++current;
                continue;
            }

            return grab::fail( grab::ErrorCode::invalid_argument,
                               "unknown option for watch: " + std::string{ arg } );
        }

        if( !options.has_window || !options.has_output || options.output.empty() )
        {
            return grab::fail( grab::ErrorCode::invalid_argument,
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
            case CaptureTarget::window :
                return screen.window_by_class( { options.wm_class } );
            case CaptureTarget::display :
                return screen.display();
            case CaptureTarget::region :
                return screen.region(
                    static_cast<std::int16_t>( options.region.x ),
                    static_cast<std::int16_t>( options.region.y ),
                    static_cast<std::uint16_t>( options.region.width ),
                    static_cast<std::uint16_t>( options.region.height )
                );
            case CaptureTarget::none :
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "capture target is missing" );
        }
        return grab::fail( grab::ErrorCode::internal_fault,
                           "capture target is invalid" );
    }

    [[nodiscard]]
    grab::Result<void>
    write_png_file( const std::filesystem::path& path,
                    std::span<const std::byte>   bytes )
    {
        if( bytes.size() >
            static_cast<std::size_t>( std::numeric_limits<std::streamsize>::max() ) )
        {
            return grab::fail( grab::ErrorCode::invalid_argument,
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
            return grab::fail( grab::ErrorCode::provider_failed,
                               "failed to open output file: " + path.string() );
        }

        if( !output_bytes.empty() )
        {
            stream.write( output_bytes.data(),
                          static_cast<std::streamsize>( output_bytes.size() ) );
        }
        if( !stream )
        {
            return grab::fail( grab::ErrorCode::provider_failed,
                               "failed to write output file: " + path.string() );
        }

        return {};
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
        ( void )std::fputc( kDimensionSeparator, stdout );
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
            return kUsageError;
        }

        auto input = grab::Input::open( options->display, options->layout );
        if( !input.has_value() )
        {
            print_fatal( input.error().message.c_str() );
            return kRuntimeError;
        }

        auto result = input->type_text( options->text );
        if( !result.has_value() )
        {
            print_fatal( result.error().message.c_str() );
            return kRuntimeError;
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
            return kUsageError;
        }

        auto input = grab::Input::open( options->display );
        if( !input.has_value() )
        {
            print_fatal( input.error().message.c_str() );
            return kRuntimeError;
        }

        auto result = input->click_at( static_cast<std::int16_t>( options->at.x ),
                                       static_cast<std::int16_t>( options->at.y ),
                                       options->button );
        if( !result.has_value() )
        {
            print_fatal( result.error().message.c_str() );
            return kRuntimeError;
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
            return kUsageError;
        }

        auto input = grab::Input::open( options->display );
        if( !input.has_value() )
        {
            print_fatal( input.error().message.c_str() );
            return kRuntimeError;
        }

        auto result = input->drag( options->from, options->to );
        if( !result.has_value() )
        {
            print_fatal( result.error().message.c_str() );
            return kRuntimeError;
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
            return kUsageError;
        }

        auto screen = grab::Screen::open();
        if( !screen.has_value() )
        {
            print_fatal( screen.error().message.c_str() );
            return kRuntimeError;
        }

        auto image = capture_image( *screen, *options );
        if( !image.has_value() )
        {
            print_fatal( image.error().message.c_str() );
            return kRuntimeError;
        }

        auto encoded = grab::codec::encode_png( *image );
        if( !encoded.has_value() )
        {
            print_fatal( encoded.error().message.c_str() );
            return kRuntimeError;
        }

        auto written = write_png_file( options->output, *encoded );
        if( !written.has_value() )
        {
            print_fatal( written.error().message.c_str() );
            return kRuntimeError;
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
            return kUsageError;
        }

        auto screen = grab::Screen::open();
        if( !screen.has_value() )
        {
            print_fatal( screen.error().message.c_str() );
            return kRuntimeError;
        }

        auto result = grab::screen::batch_capture( *screen, options->items );
        if( !result.has_value() )
        {
            print_fatal( result.error().message.c_str() );
            return kRuntimeError;
        }

        print_batch_success( *result );
        return result->misses.empty() ? 0 : kRuntimeError;
    }

    int
    run_compare_command( std::span<char*> args )
    {
        auto options = parse_compare_options( args );
        if( !options.has_value() )
        {
            print_error( options.error().message );
            print_usage();
            return kUsageError;
        }

        std::optional<grab::notify::Notifier> notifier;
        if( options->notify )
        {
            auto opened = grab::notify::Notifier::open();
            if( !opened.has_value() )
            {
                print_fatal( opened.error().message.c_str() );
                return kRuntimeError;
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
            return kRuntimeError;
        }

        print_compare_success( *diff );
        return diff->diff_pixels == kNoDiffPixels ? 0 : kRuntimeError;
    }

    int
    run_watch_command( std::span<char*> args )
    {
        auto options = parse_watch_options( args );
        if( !options.has_value() )
        {
            print_error( options.error().message );
            print_usage();
            return kUsageError;
        }

        sigset_t signals{};
        if( !configure_daemon_signal_mask( signals ) )
        {
            print_fatal( "failed to configure watch signal handling" );
            return kRuntimeError;
        }

        auto screen = grab::Screen::open();
        if( !screen.has_value() )
        {
            restore_daemon_signal_mask( signals );
            print_fatal( screen.error().message.c_str() );
            return kRuntimeError;
        }

        ( void )std::fputs( "watching for title changes; press Ctrl-C to stop\n",
                            stdout );
        auto captured =
            grab::screen::watch_capture( *screen,
                                         std::vector<std::string>{ options->wm_class },
                                         options->output.string(),
                                         [&signals]
                                         {
                                             return watch_signal_pending( signals );
                                         } );

        restore_daemon_signal_mask( signals );
        if( !captured.has_value() )
        {
            print_fatal( captured.error().message.c_str() );
            return kRuntimeError;
        }

        print_watch_success( *captured );
        return 0;
    }

    int
    run_daemon_command( std::span<char*> args )
    {
        grab::service::DaemonOptions options;
        if( !parse_daemon_options( args, options ) )
        {
            print_usage();
            return kUsageError;
        }

        sigset_t signals{};
        if( !configure_daemon_signal_mask( signals ) )
        {
            print_fatal( "failed to configure daemon signal handling" );
            return kRuntimeError;
        }

        auto daemon = grab::service::Daemon::start( std::move( options ) );
        if( !daemon.has_value() )
        {
            restore_daemon_signal_mask( signals );
            print_fatal( daemon.error().message.c_str() );
            return kRuntimeError;
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
            return kUsageError;
        }

        const std::span<char*>     args( argv, static_cast<std::size_t>( argc ) );
        const auto                 cli_args = args.subspan( 1 );
        const std::string_view     command  = cli_args.front();
        constexpr std::string_view kDoctorCommand{ "doctor" };
        constexpr std::string_view kDaemonCommand{ "daemon" };
        constexpr std::string_view kTypeCommand{ "type" };
        constexpr std::string_view kClickCommand{ "click" };
        constexpr std::string_view kDragCommand{ "drag" };
        constexpr std::string_view kCaptureCommand{ "capture" };
        constexpr std::string_view kBatchCommand{ "batch" };
        constexpr std::string_view kCompareCommand{ "compare" };
        constexpr std::string_view kWatchCommand{ "watch" };
        constexpr std::string_view kKeyCommand{ "key" };
        constexpr std::string_view kDragCurveCommand{ "drag-curve" };
        constexpr std::string_view kSessionCommand{ "session" };
        constexpr std::string_view kJsonFlag{ "--json" };

        if( command == kDoctorCommand )
        {
            bool as_json = false;
            for( const char* arg : cli_args.subspan( 1 ) )
            {
                if( std::string_view( arg ) == kJsonFlag )
                {
                    as_json = true;
                }
                else
                {
                    print_usage();
                    return kUsageError;
                }
            }
            return run_doctor_command( as_json );
        }

        if( command == kDaemonCommand )
        {
            return run_daemon_command( cli_args.subspan( 1 ) );
        }

        if( command == kTypeCommand )
        {
            return run_type_command( cli_args.subspan( 1 ) );
        }

        if( command == kClickCommand )
        {
            return run_click_command( cli_args.subspan( 1 ) );
        }

        if( command == kDragCommand )
        {
            return run_drag_command( cli_args.subspan( 1 ) );
        }

        if( command == kCaptureCommand )
        {
            return run_capture_command( cli_args.subspan( 1 ) );
        }

        if( command == kBatchCommand )
        {
            return run_batch_command( cli_args.subspan( 1 ) );
        }

        if( command == kCompareCommand )
        {
            return run_compare_command( cli_args.subspan( 1 ) );
        }

        if( command == kWatchCommand )
        {
            return run_watch_command( cli_args.subspan( 1 ) );
        }

        if( command == kKeyCommand )
        {
            return grab::cli::run_key_command( cli_args.subspan( 1 ) );
        }

        if( command == kDragCurveCommand )
        {
            return grab::cli::run_drag_curve_command( cli_args.subspan( 1 ) );
        }

        if( command == kSessionCommand )
        {
            std::vector<std::string_view> session_args;
            session_args.reserve( cli_args.size() - 1U );
            for( const char* arg : cli_args.subspan( 1 ) )
            {
                session_args.emplace_back( arg );
            }
            return grab::cli::run_session_command( session_args );
        }

        print_usage();
        return kUsageError;
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
