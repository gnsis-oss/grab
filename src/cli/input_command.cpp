#include "cli/input_command.hpp"
#include "core/checked.hpp"
#include "grab/result.hpp"
#include "grab/window.hpp"
#include "input/gesture.hpp"
#include "platform/x11/xcb_connection.hpp"
#include "platform/x11/xcb_window.hpp"
#include "platform/x11/xkb_keymap.hpp"
#include "platform/x11/xtest_input.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace grab::cli
{

    namespace
    {

        constexpr int              success_exit_code = 0;
        constexpr int              error_exit_code   = 1;
        constexpr std::size_t      flag_value_stride = 2U;
        constexpr std::size_t      value_offset      = 1U;
        constexpr std::size_t      single_arg_count  = 1U;
        constexpr std::string_view display_flag      = "--display";
        constexpr std::string_view window_flag       = "--window";
        constexpr std::string_view fx_flag           = "--fx";
        constexpr std::string_view fy_flag           = "--fy";
        constexpr std::string_view button_flag       = "--button";
        constexpr std::string_view text_flag         = "--text";
        constexpr std::string_view keysym_flag       = "--keysym";
        constexpr std::string_view src_flag          = "--src";
        constexpr std::string_view dst_flag          = "--dst";

        struct FlagValue
        {
                std::string_view flag;
                std::string_view value;
        };

        struct TargetOptions
        {
                std::string display;
                WindowMatch window_match;
                bool        has_window = false;
        };

        struct ClickOptions
        {
                TargetOptions target;
                double        fx     = 0.0;
                double        fy     = 0.0;
                std::uint8_t  button = grab::input::left_button;
                bool          has_fx = false;
                bool          has_fy = false;
        };

        struct TypeOptions
        {
                TargetOptions target;
                std::string   text;
                bool          has_text = false;
        };

        struct KeyOptions
        {
                TargetOptions target;
                std::string   keysym;
                bool          has_keysym = false;
        };

        struct DragCurveOptions
        {
                TargetOptions target;
                FractionPair  source;
                FractionPair  destination;
                bool          has_source      = false;
                bool          has_destination = false;
        };

        struct InputContext
        {
                grab::platform::x11::XcbConnection connection;
                WindowRef                          window;
                WindowRect                         rect;
                grab::platform::x11::XkbKeymap     keymap;
        };

        [[nodiscard]]
        grab::Result<std::string_view>
        read_arg( std::span<char* const> args,
                  std::size_t            index )
        {
            if( index >= args.size() )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "argument is missing" );
            }

            const char* const value = args.subspan( index, single_arg_count ).front();
            if( value == nullptr )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "argument is null" );
            }
            return std::string_view{ value };
        }

        [[nodiscard]]
        grab::Result<FlagValue>
        read_flag_value( std::span<char* const> args,
                         std::size_t            index )
        {
            auto flag = read_arg( args, index );
            if( !flag.has_value() )
            {
                return grab::fail( flag.error().code, flag.error().message );
            }

            const std::size_t value_index = index + value_offset;
            if( value_index >= args.size() )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   std::string{ *flag } + " requires a value" );
            }

            auto value = read_arg( args, value_index );
            if( !value.has_value() )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   std::string{ *flag } + " value is null" );
            }

            return FlagValue{
                .flag  = *flag,
                .value = *value,
            };
        }

        [[nodiscard]]
        grab::Result<std::uint8_t>
        parse_button( std::string_view input )
        {
            if( input.empty() )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "button is empty" );
            }

            std::uint32_t     value = 0U;
            const char* const first = input.data();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            const char* const last   = first + input.size();
            const auto        parsed = std::from_chars( first, last, value );
            if( parsed.ec != std::errc{} || parsed.ptr != last )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "button contains an invalid number" );
            }

            auto button =
                grab::checked_cast<std::uint8_t>( value,
                                                  grab::ErrorCode::invalid_argument,
                                                  "button is out of range" );
            if( !button.has_value() )
            {
                return grab::fail( button.error().code, button.error().message );
            }
            return *button;
        }

        [[nodiscard]]
        bool
        apply_target_option( TargetOptions&   options,
                             const FlagValue& option )
        {
            if( option.flag == display_flag )
            {
                options.display = option.value;
                return true;
            }
            if( option.flag == window_flag )
            {
                options.window_match = WindowMatch{ .app = std::string{ option.value } };
                options.has_window   = true;
                return true;
            }
            return false;
        }

        [[nodiscard]]
        grab::Result<void>
        validate_target( const TargetOptions& options )
        {
            if( !options.has_window )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "--window is required" );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        apply_click_option( ClickOptions&    options,
                            const FlagValue& option )
        {
            if( apply_target_option( options.target, option ) )
            {
                return {};
            }
            if( option.flag == fx_flag )
            {
                auto fx = detail::parse_fraction_number( option.value );
                if( !fx.has_value() )
                {
                    return grab::fail( fx.error().code, fx.error().message );
                }
                options.fx     = *fx;
                options.has_fx = true;
                return {};
            }
            if( option.flag == fy_flag )
            {
                auto fy = detail::parse_fraction_number( option.value );
                if( !fy.has_value() )
                {
                    return grab::fail( fy.error().code, fy.error().message );
                }
                options.fy     = *fy;
                options.has_fy = true;
                return {};
            }
            if( option.flag == button_flag )
            {
                auto button = parse_button( option.value );
                if( !button.has_value() )
                {
                    return grab::fail( button.error().code, button.error().message );
                }
                options.button = *button;
                return {};
            }
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "unknown click argument: " + std::string{ option.flag } );
        }

        [[nodiscard]]
        grab::Result<void>
        apply_type_option( TypeOptions&     options,
                           const FlagValue& option )
        {
            if( apply_target_option( options.target, option ) )
            {
                return {};
            }
            if( option.flag == text_flag )
            {
                options.text     = option.value;
                options.has_text = true;
                return {};
            }
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "unknown type argument: " + std::string{ option.flag } );
        }

        [[nodiscard]]
        grab::Result<void>
        apply_key_option( KeyOptions&      options,
                          const FlagValue& option )
        {
            if( apply_target_option( options.target, option ) )
            {
                return {};
            }
            if( option.flag == keysym_flag )
            {
                options.keysym     = option.value;
                options.has_keysym = true;
                return {};
            }
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "unknown key argument: " + std::string{ option.flag } );
        }

        [[nodiscard]]
        grab::Result<void>
        apply_drag_curve_option( DragCurveOptions& options,
                                 const FlagValue&  option )
        {
            if( apply_target_option( options.target, option ) )
            {
                return {};
            }
            if( option.flag == src_flag )
            {
                auto source = parse_fraction_pair( option.value );
                if( !source.has_value() )
                {
                    return grab::fail( source.error().code, source.error().message );
                }
                options.source     = *source;
                options.has_source = true;
                return {};
            }
            if( option.flag == dst_flag )
            {
                auto destination = parse_fraction_pair( option.value );
                if( !destination.has_value() )
                {
                    return grab::fail( destination.error().code,
                                       destination.error().message );
                }
                options.destination     = *destination;
                options.has_destination = true;
                return {};
            }
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "unknown drag-curve argument: " +
                                   std::string{ option.flag } );
        }

        template<typename Options,
                 typename Apply>
        [[nodiscard]]
        grab::Result<Options>
        parse_options( std::span<char* const> args,
                       Apply                  apply )
        {
            Options options;
            for( std::size_t index  = 0U; index < args.size();
                 index             += flag_value_stride )
            {
                auto option = read_flag_value( args, index );
                if( !option.has_value() )
                {
                    return grab::fail( option.error().code, option.error().message );
                }

                auto applied = apply( options, *option );
                if( !applied.has_value() )
                {
                    return grab::fail( applied.error().code, applied.error().message );
                }
            }
            return options;
        }

        [[nodiscard]]
        grab::Result<ClickOptions>
        parse_click_args( std::span<char* const> args )
        {
            auto options = parse_options<ClickOptions>( args, apply_click_option );
            if( !options.has_value() )
            {
                return grab::fail( options.error().code, options.error().message );
            }
            auto target = validate_target( options->target );
            if( !target.has_value() )
            {
                return grab::fail( target.error().code, target.error().message );
            }
            if( !options->has_fx )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "--fx is required" );
            }
            if( !options->has_fy )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "--fy is required" );
            }
            return *options;
        }

        [[nodiscard]]
        grab::Result<TypeOptions>
        parse_type_args( std::span<char* const> args )
        {
            auto options = parse_options<TypeOptions>( args, apply_type_option );
            if( !options.has_value() )
            {
                return grab::fail( options.error().code, options.error().message );
            }
            auto target = validate_target( options->target );
            if( !target.has_value() )
            {
                return grab::fail( target.error().code, target.error().message );
            }
            if( !options->has_text )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "--text is required" );
            }
            return *options;
        }

        [[nodiscard]]
        grab::Result<KeyOptions>
        parse_key_args( std::span<char* const> args )
        {
            auto options = parse_options<KeyOptions>( args, apply_key_option );
            if( !options.has_value() )
            {
                return grab::fail( options.error().code, options.error().message );
            }
            auto target = validate_target( options->target );
            if( !target.has_value() )
            {
                return grab::fail( target.error().code, target.error().message );
            }
            if( !options->has_keysym )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "--keysym is required" );
            }
            return *options;
        }

        [[nodiscard]]
        grab::Result<DragCurveOptions>
        parse_drag_curve_args( std::span<char* const> args )
        {
            auto options =
                parse_options<DragCurveOptions>( args, apply_drag_curve_option );
            if( !options.has_value() )
            {
                return grab::fail( options.error().code, options.error().message );
            }
            auto target = validate_target( options->target );
            if( !target.has_value() )
            {
                return grab::fail( target.error().code, target.error().message );
            }
            if( !options->has_source )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "--src is required" );
            }
            if( !options->has_destination )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "--dst is required" );
            }
            return *options;
        }

        [[nodiscard]]
        grab::Result<InputContext>
        make_input_context( const TargetOptions& options )
        {
            auto connection =
                grab::platform::x11::XcbConnection::open( options.display );
            if( !connection.has_value() )
            {
                return grab::fail( connection.error().code, connection.error().message );
            }

            auto window =
                grab::platform::x11::find_window( *connection, options.window_match );
            if( !window.has_value() )
            {
                return grab::fail( window.error().code, window.error().message );
            }

            auto rect = grab::platform::x11::window_geometry( *connection, *window );
            if( !rect.has_value() )
            {
                return grab::fail( rect.error().code, rect.error().message );
            }

            auto keymap = grab::platform::x11::XkbKeymap::from_connection( *connection );
            if( !keymap.has_value() )
            {
                return grab::fail( keymap.error().code, keymap.error().message );
            }

            return InputContext{
                .connection = std::move( *connection ),
                .window     = *window,
                .rect       = *rect,
                .keymap     = std::move( *keymap ),
            };
        }

        [[nodiscard]]
        grab::Result<void>
        validate_text( const grab::platform::x11::XkbKeymap& keymap,
                       std::string_view                      text )
        {
            auto strokes = keymap.strokes_for_text( text );
            if( !strokes.has_value() )
            {
                return grab::fail( strokes.error().code, strokes.error().message );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        validate_keysym( const grab::platform::x11::XkbKeymap& keymap,
                         std::string_view                      name )
        {
            auto keysym = grab::platform::x11::XkbKeymap::keysym_from_name( name );
            if( !keysym.has_value() )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "unknown keysym: " + std::string{ name } );
            }

            auto stroke = keymap.stroke_for_keysym( *keysym );
            if( !stroke.has_value() )
            {
                return grab::fail( grab::ErrorCode::unsupported_character,
                                   "keysym is not present in keymap: " +
                                       std::string{ name } );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        run_click( const ClickOptions& options )
        {
            auto context = make_input_context( options.target );
            if( !context.has_value() )
            {
                return grab::fail( context.error().code, context.error().message );
            }

            grab::platform::x11::XtestInputSink sink{
                context->connection,
                context->keymap,
                context->window,
            };
            grab::input::activate( sink );
            grab::input::click_frac( sink,
                                     context->rect,
                                     options.fx,
                                     options.fy,
                                     options.button );
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        run_type( const TypeOptions& options )
        {
            auto context = make_input_context( options.target );
            if( !context.has_value() )
            {
                return grab::fail( context.error().code, context.error().message );
            }

            auto text = validate_text( context->keymap, options.text );
            if( !text.has_value() )
            {
                return grab::fail( text.error().code, text.error().message );
            }

            grab::platform::x11::XtestInputSink sink{
                context->connection,
                context->keymap,
                context->window,
            };
            grab::input::activate( sink );
            grab::input::type_text( sink, options.text );
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        run_key( const KeyOptions& options )
        {
            auto context = make_input_context( options.target );
            if( !context.has_value() )
            {
                return grab::fail( context.error().code, context.error().message );
            }

            auto keysym = validate_keysym( context->keymap, options.keysym );
            if( !keysym.has_value() )
            {
                return grab::fail( keysym.error().code, keysym.error().message );
            }

            grab::platform::x11::XtestInputSink sink{
                context->connection,
                context->keymap,
                context->window,
            };
            grab::input::activate( sink );
            grab::input::key( sink, options.keysym );
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        run_drag_curve( const DragCurveOptions& options )
        {
            auto context = make_input_context( options.target );
            if( !context.has_value() )
            {
                return grab::fail( context.error().code, context.error().message );
            }

            grab::platform::x11::XtestInputSink sink{
                context->connection,
                context->keymap,
                context->window,
            };
            grab::input::activate( sink );
            grab::input::drag_curve( sink,
                                     context->rect,
                                     options.source.first,
                                     options.source.second,
                                     options.destination.first,
                                     options.destination.second );
            return {};
        }

        void
        print_error( std::string_view message )
        {
            ( void )std::fputs( "grab: ", stderr );
            ( void )std::fputs( std::string{ message }.c_str(), stderr );
            ( void )std::fputc( '\n', stderr );
        }

        int
        finish( const grab::Result<void>& result )
        {
            if( !result.has_value() )
            {
                print_error( result.error().message );
                return error_exit_code;
            }
            return success_exit_code;
        }

    }    // namespace

    int
    run_click_command( std::span<char* const> args )
    {
        auto options = parse_click_args( args );
        if( !options.has_value() )
        {
            print_error( options.error().message );
            return error_exit_code;
        }
        return finish( run_click( *options ) );
    }

    int
    run_type_command( std::span<char* const> args )
    {
        auto options = parse_type_args( args );
        if( !options.has_value() )
        {
            print_error( options.error().message );
            return error_exit_code;
        }
        return finish( run_type( *options ) );
    }

    int
    run_key_command( std::span<char* const> args )
    {
        auto options = parse_key_args( args );
        if( !options.has_value() )
        {
            print_error( options.error().message );
            return error_exit_code;
        }
        return finish( run_key( *options ) );
    }

    int
    run_drag_curve_command( std::span<char* const> args )
    {
        auto options = parse_drag_curve_args( args );
        if( !options.has_value() )
        {
            print_error( options.error().message );
            return error_exit_code;
        }
        return finish( run_drag_curve( *options ) );
    }

}    // namespace grab::cli
