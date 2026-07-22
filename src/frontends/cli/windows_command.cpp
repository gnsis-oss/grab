#include "drivers/desktop/x11/window_match.hpp"
#include "frontends/cli/capture_command.hpp"
#include "frontends/cli/common.hpp"
#include "frontends/cli/windows_command.hpp"
#include "grab/geometry/rectangle.hpp"
#include "grab/result.hpp"
#include "grab/screen.hpp"
#include "grab/window_info.hpp"
#include "kernel/support/ascii.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab::cli
{

    namespace
    {

        constexpr std::string_view jsonFlag     = "--json";
        constexpr std::string_view classFlag    = "--class";
        constexpr std::string_view windowFlag   = "--window";
        constexpr std::string_view windowIdFlag = "--window-id";
        constexpr std::string_view displayFlag  = "--display";
        constexpr std::string_view typeFlag     = "--type";
        constexpr std::string_view geometryFlag = "--geometry";
        constexpr std::string_view timeoutFlag  = "--timeout";
        constexpr std::string_view windowsUsage =
            "usage: grab windows [--json] [--class WMCLASS] [--type TYPE] "
            "[--display D]";
        constexpr std::string_view focusUsage =
            "usage: grab focus (--window WMCLASS | --window-id ID) [--display D]";
        constexpr std::string_view placeUsage =
            "usage: grab place (--window WMCLASS | --window-id ID) "
            "--geometry WxH+X+Y [--display D] [--timeout MS]";
        constexpr int           jsonIndentWidth = 2;
        // An hour of settling is already absurd; the cap only keeps the parse from
        // silently wrapping a wildly out-of-range value.
        constexpr std::uint32_t maxTimeoutMs = 3'600'000U;

        // Consumes `--flag VALUE`, advancing past the value. Returns false when the
        // flag is the last argument, which is the "missing value" usage error.
        [[nodiscard]]
        bool
        take_value( std::span<char* const> args,
                    std::size_t&           index,
                    std::string&           target )
        {
            if( index + 1U >= args.size() )
            {
                return false;
            }
            ++index;
            target = args[index];
            return true;
        }

        void
        print_line( std::string_view line )
        {
            ( void )std::fwrite( line.data(), sizeof( char ), line.size(), stderr );
            ( void )std::fputc( '\n', stderr );
        }

        [[nodiscard]]
        grab::Result<grab::Screen>
        open_screen( const std::string& display )
        {
            return grab::Screen::open( display.empty() ? nullptr : display.c_str() );
        }

        int
        report( const grab::Error& error )
        {
            print_error( error.message );
            return runtimeExitCode;
        }

        // Applies `--window` / `--window-id`, which are mutually exclusive: taking
        // both would leave the caller guessing which selector actually won.
        [[nodiscard]]
        grab::Result<bool>
        apply_selector_flag( WindowSelector&        selector,
                             std::string_view       flag,
                             std::span<char* const> args,
                             std::size_t&           index )
        {
            if( flag != windowFlag && flag != windowIdFlag )
            {
                return false;
            }
            if( !selector.wm_class.empty() || selector.window_id.has_value() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "--window and --window-id are mutually exclusive" );
            }

            std::string value;
            if( !take_value( args, index, value ) )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   std::string{ flag } + " requires a value" );
            }
            if( flag == windowFlag )
            {
                selector.wm_class = std::move( value );
                return true;
            }

            auto parsed =
                detail::parse_unsigned( value,
                                        std::numeric_limits<std::uint32_t>::max() );
            if( !parsed.has_value() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "--window-id must be a decimal window id" );
            }
            selector.window_id = *parsed;
            return true;
        }

        [[nodiscard]]
        grab::Result<void>
        validate_selector( const WindowSelector& selector,
                           std::string_view      verb )
        {
            if( selector.wm_class.empty() && !selector.window_id.has_value() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   std::string{ verb } +
                                       " requires --window or --window-id" );
            }
            return {};
        }

        // Splits one `+N` / `-N` / `+-N` offset off the front of a geometry string.
        // The separator carries the sign, so `+120-40` and `+120+-40` both mean
        // (120, -40); an X11 geometry's "measured from the far edge" reading of a
        // bare `-` is deliberately not supported, because a placement request has
        // to be one unambiguous coordinate.
        [[nodiscard]]
        grab::Result<std::int16_t>
        take_offset( std::string_view& input )
        {
            if( input.empty() || ( input.front() !=
                                   detail::offset_marker &&
                                   input.front() != detail::minus_sign ) )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "geometry must match WxH+X+Y" );
            }
            bool negative = input.front() == detail::minus_sign;
            input.remove_prefix( 1U );
            if( !input.empty() && input.front() == detail::minus_sign )
            {
                negative = !negative;
                input.remove_prefix( 1U );
            }

            std::size_t digits = 0U;
            while( digits < input.size() && detail::is_digit( input[digits] ) )
            {
                ++digits;
            }
            const std::string token =
                ( negative ? std::string{ detail::minus_sign } : std::string{} ) +
                std::string{ input.substr( 0U, digits ) };
            input.remove_prefix( digits );
            return detail::parse_signed_i16( token );
        }

        // Enumerates once and resolves the selector against that snapshot, so the
        // id an error message names is the id the caller could have seen.
        [[nodiscard]]
        grab::Result<std::uint32_t>
        resolve_selector( grab::Screen&         screen,
                          const WindowSelector& selector )
        {
            auto windows = screen.windows();
            if( !windows.has_value() )
            {
                return std::unexpected( std::move( windows.error() ) );
            }
            return select_window_id( selector, *windows );
        }

    }    // namespace

    std::string
    format_geometry( const grab::geometry::Rectangle& bounds )
    {
        const std::string x_offset =
            bounds.x < 0
                ? std::to_string( bounds.x )
                : std::string{ detail::offset_marker } + std::to_string( bounds.x );
        const std::string y_offset =
            bounds.y < 0
                ? std::to_string( bounds.y )
                : std::string{ detail::offset_marker } + std::to_string( bounds.y );
        return std::to_string( bounds.width ) +
               std::string{ detail::dimension_marker } +
               std::to_string( bounds.height ) +
               x_offset +
               y_offset;
    }

    grab::Result<grab::geometry::Rectangle>
    parse_placement_geometry( std::string_view input )
    {
        const std::size_t dimension = input.find( detail::dimension_marker );
        if( dimension == std::string_view::npos )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "geometry must match WxH+X+Y" );
        }

        auto width = detail::parse_nonzero_u16( input.substr( 0U, dimension ) );
        if( !width.has_value() )
        {
            return std::unexpected( std::move( width.error() ) );
        }

        constexpr std::array<char, 2U> offsetMarkers{
            detail::offset_marker,
            detail::minus_sign
        };
        std::string_view  rest   = input.substr( dimension + 1U );
        const std::size_t offset = rest.find_first_of(
            std::string_view{ offsetMarkers.data(), offsetMarkers.size() }
        );
        if( offset == std::string_view::npos )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "geometry must match WxH+X+Y" );
        }

        auto height = detail::parse_nonzero_u16( rest.substr( 0U, offset ) );
        if( !height.has_value() )
        {
            return std::unexpected( std::move( height.error() ) );
        }

        rest   = rest.substr( offset );
        auto x = take_offset( rest );
        if( !x.has_value() )
        {
            return std::unexpected( std::move( x.error() ) );
        }
        auto y = take_offset( rest );
        if( !y.has_value() )
        {
            return std::unexpected( std::move( y.error() ) );
        }
        if( !rest.empty() )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "geometry has trailing characters" );
        }

        return grab::geometry::Rectangle{
            .x      = *x,
            .y      = *y,
            .width  = *width,
            .height = *height,
        };
    }

    grab::Result<std::uint32_t>
    select_window_id( const WindowSelector&                   selector,
                      const std::vector<grab::WindowSummary>& windows )
    {
        if( selector.window_id.has_value() )
        {
            const auto found = std::ranges::find( windows,
                                                  *selector.window_id,
                                                  &grab::WindowSummary::id );
            if( found == windows.end() )
            {
                return grab::fail( grab::ErrorCode::WindowNotFound,
                                   "no window has id " +
                                       std::to_string( *selector.window_id ) );
            }
            return found->id;
        }

        const auto matched = filter_windows_by_class( windows, selector.wm_class );
        if( matched.empty() )
        {
            return grab::fail( grab::ErrorCode::WindowNotFound,
                               "no window matched the requested WM_CLASS" );
        }
        return matched.front().id;
    }

    grab::Result<WindowsOptions>
    parse_windows_options( std::span<char* const> args )
    {
        WindowsOptions options;
        for( std::size_t index = 0U; index < args.size(); ++index )
        {
            const std::string_view argument{ args[index] };
            if( argument == jsonFlag )
            {
                options.as_json = true;
            }
            else if( argument == classFlag )
            {
                if( !take_value( args, index, options.wm_class ) )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--class requires a WM_CLASS value" );
                }
            }
            else if( argument == typeFlag )
            {
                if( !take_value( args, index, options.type ) )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--type requires an EWMH window type" );
                }
            }
            else if( argument == displayFlag )
            {
                if( !take_value( args, index, options.display ) )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--display requires a display value" );
                }
            }
            else
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "unknown windows option: " +
                                       std::string{ argument } );
            }
        }
        return options;
    }

    grab::Result<FocusOptions>
    parse_focus_options( std::span<char* const> args )
    {
        FocusOptions options;
        for( std::size_t index = 0U; index < args.size(); ++index )
        {
            const std::string_view argument{ args[index] };
            auto                   selected =
                apply_selector_flag( options.selector, argument, args, index );
            if( !selected.has_value() )
            {
                return std::unexpected( std::move( selected.error() ) );
            }
            if( *selected )
            {
                continue;
            }
            if( argument == displayFlag )
            {
                if( !take_value( args, index, options.display ) )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--display requires a display value" );
                }
                continue;
            }
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "unknown focus option: " + std::string{ argument } );
        }

        auto valid = validate_selector( options.selector, "focus" );
        if( !valid.has_value() )
        {
            return std::unexpected( std::move( valid.error() ) );
        }
        return options;
    }

    grab::Result<PlaceOptions>
    parse_place_options( std::span<char* const> args )
    {
        PlaceOptions options;
        bool         has_geometry = false;
        for( std::size_t index = 0U; index < args.size(); ++index )
        {
            const std::string_view argument{ args[index] };
            auto                   selected =
                apply_selector_flag( options.selector, argument, args, index );
            if( !selected.has_value() )
            {
                return std::unexpected( std::move( selected.error() ) );
            }
            if( *selected )
            {
                continue;
            }
            if( argument == displayFlag )
            {
                if( !take_value( args, index, options.display ) )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--display requires a display value" );
                }
                continue;
            }
            if( argument == geometryFlag )
            {
                std::string value;
                if( !take_value( args, index, value ) )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--geometry requires a WxH+X+Y value" );
                }
                auto geometry = parse_placement_geometry( value );
                if( !geometry.has_value() )
                {
                    return std::unexpected( std::move( geometry.error() ) );
                }
                options.geometry = *geometry;
                has_geometry     = true;
                continue;
            }
            if( argument == timeoutFlag )
            {
                std::string value;
                if( !take_value( args, index, value ) )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--timeout requires a millisecond value" );
                }
                auto milliseconds = detail::parse_unsigned( value, maxTimeoutMs );
                if( !milliseconds.has_value() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--timeout must be a millisecond count" );
                }
                options.timeout = std::chrono::milliseconds{ *milliseconds };
                continue;
            }
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "unknown place option: " + std::string{ argument } );
        }

        auto valid = validate_selector( options.selector, "place" );
        if( !valid.has_value() )
        {
            return std::unexpected( std::move( valid.error() ) );
        }
        if( !has_geometry )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "place requires --geometry WxH+X+Y" );
        }
        return options;
    }

    std::vector<grab::WindowSummary>
    filter_windows_by_class( std::vector<grab::WindowSummary> windows,
                             const std::string&               wm_class )
    {
        if( wm_class.empty() )
        {
            return windows;
        }
        const std::vector<std::string> candidates =
            grab::screen::normalized_wm_class_candidates( { wm_class } );
        const auto removed = std::ranges::remove_if(
            windows,
            [&candidates]( const grab::WindowSummary& window )
            {
                return !grab::screen::wm_class_matches_any( window.wm_class,
                                                            candidates );
            }
        );
        windows.erase( removed.begin(), removed.end() );
        return windows;
    }

    std::vector<grab::WindowSummary>
    filter_windows_by_type( std::vector<grab::WindowSummary> windows,
                            const std::string&               type )
    {
        if( type.empty() )
        {
            return windows;
        }
        const std::string wanted  = grab::core::ascii_lower_copy( type );
        const auto        removed = std::ranges::remove_if(
            windows,
            [&wanted]( const grab::WindowSummary& window )
            {
                return grab::core::ascii_lower_copy( window.type ) != wanted;
            }
        );
        windows.erase( removed.begin(), removed.end() );
        return windows;
    }

    std::string
    format_windows_json( const std::vector<grab::WindowSummary>& windows )
    {
        nlohmann::ordered_json array = nlohmann::ordered_json::array();
        for( const grab::WindowSummary& window : windows )
        {
            nlohmann::ordered_json entry;
            entry["id"]       = window.id;
            entry["wm_class"] = window.wm_class;
            entry["title"]    = window.title;
            entry["type"]     = window.type;
            // Assigned field-by-field rather than through a brace-initializer so
            // that an absent pid lands as JSON null instead of a one-element array.
            if( window.pid.has_value() )
            {
                entry["pid"] = *window.pid;
            }
            else
            {
                entry["pid"] = nullptr;
            }
            entry["x"]      = window.bounds.x;
            entry["y"]      = window.bounds.y;
            entry["width"]  = window.bounds.width;
            entry["height"] = window.bounds.height;
            array.push_back( std::move( entry ) );
        }
        return array.dump( jsonIndentWidth );
    }

    std::string
    format_windows_text( const std::vector<grab::WindowSummary>& windows )
    {
        std::string text;
        for( const grab::WindowSummary& window : windows )
        {
            text += std::to_string( window.id );
            text += ' ';
            text += window.wm_class.empty() ? "-" : window.wm_class;
            text += ' ';
            text += window.type.empty() ? "-" : window.type;
            text += " pid=";
            text += window.pid.has_value() ? std::to_string( *window.pid ) : "-";
            text += ' ';
            text += std::to_string( window.bounds.x );
            text += ',';
            text += std::to_string( window.bounds.y );
            text += ' ';
            text += std::to_string( window.bounds.width );
            text += 'x';
            text += std::to_string( window.bounds.height );
            text += " \"";
            text += window.title;
            text += "\"\n";
        }
        return text;
    }

    int
    run_windows_command( std::span<char* const> args )
    {
        auto options = parse_windows_options( args );
        if( !options.has_value() )
        {
            print_error( options.error().message );
            print_line( windowsUsage );
            return usageExitCode;
        }

        auto screen = open_screen( options->display );
        if( !screen.has_value() )
        {
            return report( screen.error() );
        }

        auto windows = screen->windows();
        if( !windows.has_value() )
        {
            return report( windows.error() );
        }

        const auto selected =
            filter_windows_by_type( filter_windows_by_class( std::move( *windows ),
                                                             options->wm_class ),
                                    options->type );
        const std::string output = options->as_json ? format_windows_json( selected )
                                                    : format_windows_text( selected );
        ( void )std::fputs( output.c_str(), stdout );
        if( options->as_json )
        {
            ( void )std::fputc( '\n', stdout );
        }
        return successExitCode;
    }

    int
    run_focus_command( std::span<char* const> args )
    {
        auto options = parse_focus_options( args );
        if( !options.has_value() )
        {
            print_error( options.error().message );
            print_line( focusUsage );
            return usageExitCode;
        }

        auto screen = open_screen( options->display );
        if( !screen.has_value() )
        {
            return report( screen.error() );
        }

        auto target = resolve_selector( *screen, options->selector );
        if( !target.has_value() )
        {
            return report( target.error() );
        }

        auto focused = screen->activate_window( *target );
        if( !focused.has_value() )
        {
            return report( focused.error() );
        }
        return successExitCode;
    }

    int
    run_place_command( std::span<char* const> args )
    {
        auto options = parse_place_options( args );
        if( !options.has_value() )
        {
            print_error( options.error().message );
            print_line( placeUsage );
            return usageExitCode;
        }

        auto screen = open_screen( options->display );
        if( !screen.has_value() )
        {
            return report( screen.error() );
        }

        auto target = resolve_selector( *screen, options->selector );
        if( !target.has_value() )
        {
            return report( target.error() );
        }

        // Focus first: a window manager that keeps the window in a tiled or
        // "recently maximised" mode is far more likely to honour geometry for the
        // window it considers active.
        auto focused = screen->activate_window( *target );
        if( !focused.has_value() )
        {
            return report( focused.error() );
        }

        auto placed =
            screen->place_window( *target, options->geometry, options->timeout );
        if( !placed.has_value() )
        {
            return report( placed.error() );
        }

        const std::string achieved =
            std::to_string( *target ) + " " + format_geometry( *placed ) + "\n";
        ( void )std::fputs( achieved.c_str(), stdout );
        return successExitCode;
    }

}    // namespace grab::cli
