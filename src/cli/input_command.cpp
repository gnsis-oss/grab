#include "cli/common.hpp"
#include "cli/input_command.hpp"
#include "grab/drag.hpp"
#include "grab/geometry/point.hpp"
#include "grab/input.hpp"
#include "grab/interaction.hpp"
#include "grab/locator.hpp"
#include "grab/query.hpp"
#include "grab/result.hpp"
#include "grab/role.hpp"
#include "grab/session.hpp"
#include "grab/ui.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <xkbcommon/xkbcommon.h>

namespace grab::cli
{

    namespace
    {

        constexpr char             pointSeparator  = ',';
        constexpr xkb_keysym_t     noSymbol        = 0U;
        constexpr std::string_view displayFlag     = "--display";
        constexpr std::string_view destinationFlag = "--dst";
        constexpr std::string_view keysymFlag      = "--keysym";
        constexpr std::string_view layoutFlag      = "--layout";
        constexpr std::string_view sourceFlag      = "--src";
        constexpr std::string_view windowFlag      = "--window";

        struct TargetOptions
        {
                std::string display;
                std::string window;
                bool        has_window = false;
        };

        struct KeyOptions
        {
                TargetOptions target;
                std::string   keysym;
                std::string   layout;
                bool          has_keysym = false;
                bool          has_layout = false;
        };

        struct FractionPoint
        {
                double x = 0.0;
                double y = 0.0;
        };

        struct DragCurveOptions
        {
                TargetOptions target;
                FractionPoint source;
                FractionPoint destination;
                bool          has_source      = false;
                bool          has_destination = false;
        };

        struct FlagValue
        {
                std::string_view flag;
                std::string_view value;
        };

        [[nodiscard]]
        grab::Result<std::string_view>
        argument_at( std::span<char* const> args,
                     std::size_t            index )
        {
            if( index >= args.size() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "command argument is missing" );
            }
            const char* const argument = args.subspan( index, 1U ).front();
            if( argument == nullptr )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "command argument is missing" );
            }
            return std::string_view{ argument };
        }

        [[nodiscard]]
        grab::Result<FlagValue>
        flag_value_at( std::span<char* const> args,
                       std::size_t            index )
        {
            auto flag = argument_at( args, index );
            if( !flag.has_value() )
            {
                return std::unexpected( std::move( flag.error() ) );
            }
            auto value = argument_at( args, index + 1U );
            if( !value.has_value() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   std::string{ *flag } + " requires a value" );
            }
            return FlagValue{ .flag = *flag, .value = *value };
        }

        [[nodiscard]]
        bool
        apply_target_option( TargetOptions&   target,
                             const FlagValue& option )
        {
            if( option.flag == displayFlag )
            {
                target.display = option.value;
                return true;
            }
            if( option.flag == windowFlag )
            {
                target.window     = option.value;
                target.has_window = true;
                return true;
            }
            return false;
        }

        [[nodiscard]]
        grab::Result<void>
        validate_target( const TargetOptions& target )
        {
            if( !target.has_window || target.window.empty() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "--window is required" );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<double>
        parse_fraction( std::string_view input )
        {
            if( input.empty() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "fraction is empty" );
            }

            double            value = 0.0;
            const char* const first = input.data();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            const char* const last   = first + input.size();
            const auto        parsed = std::from_chars( first, last, value );
            if( parsed.ec !=
                std::errc{} ||
                parsed.ptr !=
                last ||
                !std::isfinite( value ) )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "fraction is not a finite number" );
            }
            return value;
        }

        [[nodiscard]]
        grab::Result<FractionPoint>
        parse_fraction_point( std::string_view input )
        {
            const std::size_t separator = input.find( pointSeparator );
            if( separator ==
                std::string_view::npos ||
                input.find( pointSeparator, separator + 1U ) != std::string_view::npos )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "fraction point must match X,Y" );
            }

            auto x = parse_fraction( input.substr( 0U, separator ) );
            if( !x.has_value() )
            {
                return std::unexpected( std::move( x.error() ) );
            }
            auto y = parse_fraction( input.substr( separator + 1U ) );
            if( !y.has_value() )
            {
                return std::unexpected( std::move( y.error() ) );
            }
            return FractionPoint{ .x = *x, .y = *y };
        }

        [[nodiscard]]
        bool
        is_known_keysym( std::string_view name )
        {
            const std::string name_storage{ name };
            return xkb_keysym_from_name( name_storage.c_str(), XKB_KEYSYM_NO_FLAGS ) !=
                   noSymbol;
        }

        [[nodiscard]]
        grab::Result<KeyOptions>
        parse_key_options( std::span<char* const> args )
        {
            KeyOptions options;
            for( std::size_t index = 0U; index < args.size(); index += 2U )
            {
                auto option = flag_value_at( args, index );
                if( !option.has_value() )
                {
                    return std::unexpected( std::move( option.error() ) );
                }

                if( apply_target_option( options.target, *option ) )
                {
                    continue;
                }
                if( option->flag == keysymFlag )
                {
                    options.keysym     = option->value;
                    options.has_keysym = true;
                    continue;
                }
                if( option->flag == layoutFlag )
                {
                    options.layout     = option->value;
                    options.has_layout = true;
                    continue;
                }
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "unknown option for key: " +
                                       std::string{ option->flag } );
            }

            auto target = validate_target( options.target );
            if( !target.has_value() )
            {
                return std::unexpected( std::move( target.error() ) );
            }
            if( !options.has_keysym || options.keysym.empty() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "key requires --keysym" );
            }
            if( !is_known_keysym( options.keysym ) )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "unknown keysym: " + options.keysym );
            }
            if( options.has_layout && options.layout.empty() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "--layout must not be empty" );
            }
            return options;
        }

        [[nodiscard]]
        grab::Result<DragCurveOptions>
        parse_drag_curve_options( std::span<char* const> args )
        {
            DragCurveOptions options;
            for( std::size_t index = 0U; index < args.size(); index += 2U )
            {
                auto option = flag_value_at( args, index );
                if( !option.has_value() )
                {
                    return std::unexpected( std::move( option.error() ) );
                }

                if( apply_target_option( options.target, *option ) )
                {
                    continue;
                }
                if( option->flag == sourceFlag )
                {
                    auto point = parse_fraction_point( option->value );
                    if( !point.has_value() )
                    {
                        return std::unexpected( std::move( point.error() ) );
                    }
                    options.source     = *point;
                    options.has_source = true;
                    continue;
                }
                if( option->flag == destinationFlag )
                {
                    auto point = parse_fraction_point( option->value );
                    if( !point.has_value() )
                    {
                        return std::unexpected( std::move( point.error() ) );
                    }
                    options.destination     = *point;
                    options.has_destination = true;
                    continue;
                }
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "unknown option for drag-curve: " +
                                       std::string{ option->flag } );
            }

            auto target = validate_target( options.target );
            if( !target.has_value() )
            {
                return std::unexpected( std::move( target.error() ) );
            }
            if( !options.has_source || !options.has_destination )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "drag-curve requires --src and --dst" );
            }
            return options;
        }

        [[nodiscard]]
        grab::Result<grab::Input>
        open_input( const TargetOptions& target,
                    std::string_view     layout = {} )
        {
            const char* const display =
                target.display.empty() ? nullptr : target.display.c_str();
            return grab::Input::open( display, layout );
        }

        [[nodiscard]]
        grab::Locator
        window_locator( const std::string& wm_class )
        {
            return grab::sel::all( {
                grab::sel::role( grab::role::window ),
                grab::sel::property( grab::property::window_class, wm_class ),
            } );
        }

        [[nodiscard]]
        grab::Result<std::unique_ptr<grab::Session>>
        open_window_session( const TargetOptions& target )
        {
            grab::SessionOptions options;
            if( !target.display.empty() )
            {
                options.display = target.display;
            }
            return grab::Session::open( std::move( options ) );
        }

        [[nodiscard]]
        grab::Result<grab::Match>
        resolve_and_activate( grab::Session&       session,
                              const TargetOptions& target )
        {
            auto match = session.resolve( window_locator( target.window ) );
            if( !match.has_value() )
            {
                return std::unexpected( std::move( match.error() ) );
            }
            auto activated = session.perform( grab::Activate{ .target = *match } );
            if( !activated.has_value() )
            {
                return std::unexpected( std::move( activated.error() ) );
            }
            return *match;
        }

        [[nodiscard]]
        grab::Result<void>
        press_key( const KeyOptions& options )
        {
            auto session = open_window_session( options.target );
            if( !session.has_value() )
            {
                return std::unexpected( std::move( session.error() ) );
            }
            auto match = resolve_and_activate( **session, options.target );
            if( !match.has_value() )
            {
                return std::unexpected( std::move( match.error() ) );
            }

            // The keypress itself stays on the raw seat so --layout/--display apply.
            auto input =
                open_input( options.target,
                            options.has_layout ? options.layout : std::string_view{} );
            if( !input.has_value() )
            {
                return std::unexpected( std::move( input.error() ) );
            }
            return input->press_key( options.keysym );
        }

        [[nodiscard]]
        grab::Result<void>
        drag_curve( const DragCurveOptions& options )
        {
            auto session = open_window_session( options.target );
            if( !session.has_value() )
            {
                return std::unexpected( std::move( session.error() ) );
            }
            auto match = resolve_and_activate( **session, options.target );
            if( !match.has_value() )
            {
                return std::unexpected( std::move( match.error() ) );
            }
            auto info = ( *session )->describe( *match );
            if( !info.has_value() )
            {
                return std::unexpected( std::move( info.error() ) );
            }

            auto from_x = grab::cli::window_fraction_to_coordinate( info->bounds.x,
                                                                    info->bounds.w,
                                                                    options.source.x,
                                                                    "source x" );
            if( !from_x.has_value() )
            {
                return std::unexpected( std::move( from_x.error() ) );
            }
            auto from_y = grab::cli::window_fraction_to_coordinate( info->bounds.y,
                                                                    info->bounds.h,
                                                                    options.source.y,
                                                                    "source y" );
            if( !from_y.has_value() )
            {
                return std::unexpected( std::move( from_y.error() ) );
            }
            auto to_x = grab::cli::window_fraction_to_coordinate( info->bounds.x,
                                                                  info->bounds.w,
                                                                  options.destination.x,
                                                                  "destination x" );
            if( !to_x.has_value() )
            {
                return std::unexpected( std::move( to_x.error() ) );
            }
            auto to_y = grab::cli::window_fraction_to_coordinate( info->bounds.y,
                                                                  info->bounds.h,
                                                                  options.destination.y,
                                                                  "destination y" );
            if( !to_y.has_value() )
            {
                return std::unexpected( std::move( to_y.error() ) );
            }

            grab::input::DragOptions drag_options;
            drag_options.path = grab::input::DragOptions::Path::Cubic;
            auto receipt =
                ( *session )
                    ->perform( grab::Drag{
                        .target  = *match,
                        .from    = grab::geometry::Point{.x = *from_x, .y = *from_y},
                        .to      = grab::geometry::Point{  .x = *to_x,   .y = *to_y},
                        .options = drag_options,
            } );
            if( !receipt.has_value() )
            {
                return std::unexpected( std::move( receipt.error() ) );
            }
            return {};
        }

        int
        finish( grab::Result<void> result )
        {
            if( !result.has_value() )
            {
                print_error( result.error().message );
                return runtimeExitCode;
            }
            return successExitCode;
        }

    }    // namespace

    grab::Result<std::int16_t>
    window_fraction_to_coordinate( double           origin,
                                   double           size,
                                   double           fraction,
                                   std::string_view axis )
    {
        constexpr double minimumFraction = 0.0;
        constexpr double maximumFraction = 1.0;
        if( !std::isfinite( fraction ) ||
            fraction <
            minimumFraction ||
            fraction > maximumFraction )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               std::string{ axis } +
                                   " fraction must be between 0 and 1" );
        }
        if( !std::isfinite( origin ) )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               std::string{ axis } + " window origin must be finite" );
        }
        if( !std::isfinite( size ) || size < 1.0 )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               std::string{ axis } +
                                   " window extent must be at least one" );
        }
        const double maximum_offset = size - 1.0;
        const double absolute = std::round( origin + ( fraction * maximum_offset ) );
        if( absolute <
            static_cast<double>( std::numeric_limits<std::int16_t>::min() ) ||
            absolute > static_cast<double>( std::numeric_limits<std::int16_t>::max() ) )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               std::string{ axis } +
                                   " coordinate is outside int16 range" );
        }
        return static_cast<std::int16_t>( absolute );
    }

    int
    run_key_command( std::span<char* const> args )
    {
        auto options = parse_key_options( args );
        if( !options.has_value() )
        {
            print_error( options.error().message );
            return usageExitCode;
        }
        return finish( press_key( *options ) );
    }

    int
    run_drag_curve_command( std::span<char* const> args )
    {
        auto options = parse_drag_curve_options( args );
        if( !options.has_value() )
        {
            print_error( options.error().message );
            return usageExitCode;
        }
        return finish( drag_curve( *options ) );
    }

}    // namespace grab::cli
