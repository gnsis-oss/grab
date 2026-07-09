#include "grab/result.hpp"
#include "platform/x11/xcb_connection.hpp"
#include "platform/x11/xkb_keymap.hpp"

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ios>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format off
#include <xkbcommon/xkbcommon-x11.h>
#include <xkbcommon/xkbcommon.h>
// clang-format on

namespace grab::platform::x11
{

    namespace
    {

        constexpr xkb_layout_index_t default_layout                 = 0U;
        constexpr xkb_level_index_t  base_level                     = 0U;
        constexpr xkb_level_index_t  shift_level                    = 1U;
        constexpr xkb_keysym_t       no_symbol                      = 0U;
        constexpr int                invalid_device                 = -1;

        constexpr std::uint8_t       utf8_one_byte_limit            = 0X80U;
        constexpr std::uint8_t       utf8_two_byte_mask             = 0XE0U;
        constexpr std::uint8_t       utf8_two_byte_tag              = 0XC0U;
        constexpr std::uint8_t       utf8_two_byte_payload_mask     = 0X1FU;
        constexpr std::uint8_t       utf8_three_byte_mask           = 0XF0U;
        constexpr std::uint8_t       utf8_three_byte_tag            = 0XE0U;
        constexpr std::uint8_t       utf8_three_byte_payload_mask   = 0X0FU;
        constexpr std::uint8_t       utf8_four_byte_mask            = 0XF8U;
        constexpr std::uint8_t       utf8_four_byte_tag             = 0XF0U;
        constexpr std::uint8_t       utf8_four_byte_payload_mask    = 0X07U;
        constexpr std::uint8_t       utf8_continuation_mask         = 0XC0U;
        constexpr std::uint8_t       utf8_continuation_tag          = 0X80U;
        constexpr std::uint8_t       utf8_continuation_payload_mask = 0X3FU;
        constexpr std::uint32_t      utf8_continuation_payload_bits = 6U;
        constexpr std::uint32_t      utf8_two_byte_min              = 0X80U;
        constexpr std::uint32_t      utf8_three_byte_min            = 0X8'00U;
        constexpr std::uint32_t      utf8_four_byte_min             = 0X1'00'00U;
        constexpr std::uint32_t      unicode_surrogate_first        = 0XD8'00U;
        constexpr std::uint32_t      unicode_surrogate_last         = 0XDF'FFU;
        constexpr std::uint32_t      unicode_max                    = 0X10'FF'FFU;
        constexpr std::size_t        single_byte_count              = 1U;
        constexpr std::size_t        two_byte_count                 = 2U;
        constexpr std::size_t        three_byte_count               = 3U;
        constexpr std::size_t        four_byte_count                = 4U;
        constexpr std::size_t        first_continuation_offset      = 1U;
        constexpr int                no_syms                        = 0;
        constexpr int                min_codepoint_hex_digits       = 4;

        struct Utf8Shape
        {
                std::size_t   byte_count = 0U;
                std::uint32_t min_value  = 0U;
                std::uint32_t value      = 0U;
        };

        struct Utf8Codepoint
        {
                std::uint32_t value      = 0U;
                std::size_t   byte_count = 0U;
        };

        using XkbContextHandle =
            std::unique_ptr<xkb_context, decltype( &xkb_context_unref )>;
        using XkbKeymapHandle =
            std::unique_ptr<xkb_keymap, decltype( &xkb_keymap_unref )>;
        using XkbStateHandle = std::unique_ptr<xkb_state, decltype( &xkb_state_unref )>;

        [[nodiscard]]
        XkbContextHandle
        take_context( xkb_context* context ) noexcept
        {
            return XkbContextHandle{ context, &xkb_context_unref };
        }

        [[nodiscard]]
        XkbKeymapHandle
        take_keymap( xkb_keymap* keymap ) noexcept
        {
            return XkbKeymapHandle{ keymap, &xkb_keymap_unref };
        }

        [[nodiscard]]
        XkbStateHandle
        take_state( xkb_state* state ) noexcept
        {
            return XkbStateHandle{ state, &xkb_state_unref };
        }

        [[nodiscard]]
        bool
        masked_equals( std::uint8_t value,
                       std::uint8_t mask,
                       std::uint8_t tag ) noexcept
        {
            return ( static_cast<std::uint32_t>( value ) &
                     static_cast<std::uint32_t>( mask ) ) ==
                   static_cast<std::uint32_t>( tag );
        }

        [[nodiscard]]
        std::uint32_t
        payload_bits( std::uint8_t value,
                      std::uint8_t mask ) noexcept
        {
            return static_cast<std::uint32_t>( value ) &
                   static_cast<std::uint32_t>( mask );
        }

        [[nodiscard]]
        std::uint8_t
        byte_value( char value ) noexcept
        {
            return static_cast<std::uint8_t>( static_cast<unsigned char>( value ) );
        }

        [[nodiscard]]
        bool
        is_surrogate( std::uint32_t codepoint ) noexcept
        {
            return codepoint >=
                   unicode_surrogate_first &&
                   codepoint <= unicode_surrogate_last;
        }

        [[nodiscard]]
        grab::Result<Utf8Shape>
        utf8_shape_from_lead( std::uint8_t lead )
        {
            if( lead < utf8_one_byte_limit )
            {
                return Utf8Shape{
                    .byte_count = single_byte_count,
                    .min_value  = 0U,
                    .value      = lead,
                };
            }
            if( masked_equals( lead, utf8_two_byte_mask, utf8_two_byte_tag ) )
            {
                return Utf8Shape{
                    .byte_count = two_byte_count,
                    .min_value  = utf8_two_byte_min,
                    .value      = payload_bits( lead, utf8_two_byte_payload_mask ),
                };
            }
            if( masked_equals( lead, utf8_three_byte_mask, utf8_three_byte_tag ) )
            {
                return Utf8Shape{
                    .byte_count = three_byte_count,
                    .min_value  = utf8_three_byte_min,
                    .value      = payload_bits( lead, utf8_three_byte_payload_mask ),
                };
            }
            if( masked_equals( lead, utf8_four_byte_mask, utf8_four_byte_tag ) )
            {
                return Utf8Shape{
                    .byte_count = four_byte_count,
                    .min_value  = utf8_four_byte_min,
                    .value      = payload_bits( lead, utf8_four_byte_payload_mask ),
                };
            }
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "text contains invalid UTF-8" );
        }

        [[nodiscard]]
        grab::Result<std::uint32_t>
        append_continuations( std::string_view input,
                              Utf8Shape        shape )
        {
            std::uint32_t value = shape.value;
            for( std::size_t offset = first_continuation_offset;
                 offset < shape.byte_count;
                 ++offset )
            {
                const std::uint8_t current =
                    byte_value( input.substr( offset, single_byte_count ).front() );
                if( !masked_equals( current,
                                    utf8_continuation_mask,
                                    utf8_continuation_tag ) )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       "text contains invalid UTF-8" );
                }

                value = ( value << utf8_continuation_payload_bits ) |
                        payload_bits( current, utf8_continuation_payload_mask );
            }
            return value;
        }

        [[nodiscard]]
        grab::Result<Utf8Codepoint>
        decode_utf8_codepoint( std::string_view input )
        {
            if( input.empty() )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "text contains invalid UTF-8" );
            }

            auto shape = utf8_shape_from_lead( byte_value( input.front() ) );
            if( !shape.has_value() )
            {
                return grab::fail( shape.error().code, shape.error().message );
            }
            if( input.size() < shape->byte_count )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "text contains invalid UTF-8" );
            }

            auto value = append_continuations( input, *shape );
            if( !value.has_value() )
            {
                return grab::fail( value.error().code, value.error().message );
            }
            if( *value <
                shape->min_value ||
                is_surrogate( *value ) ||
                *value > unicode_max )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "text contains invalid UTF-8" );
            }

            return Utf8Codepoint{
                .value      = *value,
                .byte_count = shape->byte_count,
            };
        }

        [[nodiscard]]
        std::string
        codepoint_label( std::uint32_t codepoint )
        {
            std::ostringstream output;
            output << "U+" << std::uppercase << std::hex
                   << std::setw( min_codepoint_hex_digits ) << std::setfill( '0' )
                   << codepoint;
            return output.str();
        }

    }    // namespace

    XkbKeymap::XkbKeymap( xkb_context* context,
                          xkb_keymap*  keymap,
                          xkb_state*   state ) :
        context( context ),
        keymap( keymap ),
        state( state )
    {
        build_reverse_lookup();
    }

    XkbKeymap::XkbKeymap( XkbKeymap&& other ) noexcept :
        context( std::exchange( other.context,
                                nullptr ) ),
        keymap( std::exchange( other.keymap,
                               nullptr ) ),
        state( std::exchange( other.state,
                              nullptr ) ),
        reverse( std::move( other.reverse ) )
    {
    }

    XkbKeymap&
    XkbKeymap::operator=( XkbKeymap&& other ) noexcept
    {
        if( this != &other )
        {
            if( state != nullptr )
            {
                xkb_state_unref( state );
            }
            if( keymap != nullptr )
            {
                xkb_keymap_unref( keymap );
            }
            if( context != nullptr )
            {
                xkb_context_unref( context );
            }

            context = std::exchange( other.context, nullptr );
            keymap  = std::exchange( other.keymap, nullptr );
            state   = std::exchange( other.state, nullptr );
            reverse = std::move( other.reverse );
        }
        return *this;
    }

    XkbKeymap::~XkbKeymap()
    {
        if( state != nullptr )
        {
            xkb_state_unref( state );
        }
        if( keymap != nullptr )
        {
            xkb_keymap_unref( keymap );
        }
        if( context != nullptr )
        {
            xkb_context_unref( context );
        }
    }

    grab::Result<XkbKeymap>
    XkbKeymap::from_connection( const XcbConnection& conn )
    {
        auto context = take_context( xkb_context_new( XKB_CONTEXT_NO_FLAGS ) );
        if( context == nullptr )
        {
            return grab::fail( grab::ErrorCode::internal_fault,
                               "failed to create XKB context" );
        }

        const int setup =
            xkb_x11_setup_xkb_extension( conn.get(),
                                         XKB_X11_MIN_MAJOR_XKB_VERSION,
                                         XKB_X11_MIN_MINOR_XKB_VERSION,
                                         XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr );
        if( setup == 0 )
        {
            return grab::fail( grab::ErrorCode::capability_unavailable,
                               "XKB extension is unavailable on the X display" );
        }

        const int device_id = xkb_x11_get_core_keyboard_device_id( conn.get() );
        if( device_id == invalid_device )
        {
            return grab::fail( grab::ErrorCode::device_inaccessible,
                               "failed to resolve the XKB core keyboard device" );
        }

        auto keymap =
            take_keymap( xkb_x11_keymap_new_from_device( context.get(),
                                                         conn.get(),
                                                         device_id,
                                                         XKB_KEYMAP_COMPILE_NO_FLAGS ) );
        if( keymap == nullptr )
        {
            return grab::fail( grab::ErrorCode::provider_failed,
                               "failed to load the X server keymap" );
        }

        auto state = take_state(
            xkb_x11_state_new_from_device( keymap.get(), conn.get(), device_id )
        );
        if( state == nullptr )
        {
            return grab::fail( grab::ErrorCode::provider_failed,
                               "failed to load the X server keyboard state" );
        }

        return XkbKeymap{
            context.release(),
            keymap.release(),
            state.release(),
        };
    }

    grab::Result<XkbKeymap>
    XkbKeymap::from_default_names()
    {
        auto context = take_context( xkb_context_new( XKB_CONTEXT_NO_FLAGS ) );
        if( context == nullptr )
        {
            return grab::fail( grab::ErrorCode::internal_fault,
                               "failed to create XKB context" );
        }

        auto keymap =
            take_keymap( xkb_keymap_new_from_names( context.get(),
                                                    nullptr,
                                                    XKB_KEYMAP_COMPILE_NO_FLAGS ) );
        if( keymap == nullptr )
        {
            return grab::fail( grab::ErrorCode::provider_failed,
                               "failed to load the default XKB keymap" );
        }

        auto state = take_state( xkb_state_new( keymap.get() ) );
        if( state == nullptr )
        {
            return grab::fail( grab::ErrorCode::provider_failed,
                               "failed to create the default XKB state" );
        }

        return XkbKeymap{
            context.release(),
            keymap.release(),
            state.release(),
        };
    }

    std::optional<XkbKeymap::KeyStroke>
    XkbKeymap::stroke_for_keysym( xkb_keysym_t keysym ) const
    {
        const auto found = reverse.find( keysym );
        if( found == reverse.end() )
        {
            return std::nullopt;
        }
        return ( *found ).second;
    }

    std::optional<xkb_keysym_t>
    XkbKeymap::keysym_for_stroke( const KeyStroke& stroke ) const
    {
        const xkb_level_index_t level = stroke.needs_shift ? shift_level : base_level;
        const xkb_keysym_t*     syms  = nullptr;
        const int sym_count           = xkb_keymap_key_get_syms_by_level( keymap,
                                                                          stroke.keycode,
                                                                          default_layout,
                                                                          level,
                                                                          &syms );
        if( sym_count <= no_syms || syms == nullptr )
        {
            return std::nullopt;
        }
        return *syms;
    }

    grab::Result<std::vector<XkbKeymap::KeyStroke>>
    XkbKeymap::strokes_for_text( std::string_view utf8 ) const
    {
        std::vector<KeyStroke> strokes;
        strokes.reserve( utf8.size() );

        while( !utf8.empty() )
        {
            auto codepoint = decode_utf8_codepoint( utf8 );
            if( !codepoint.has_value() )
            {
                return grab::fail( codepoint.error().code, codepoint.error().message );
            }

            const xkb_keysym_t keysym = xkb_utf32_to_keysym( codepoint->value );
            auto               stroke = stroke_for_keysym( keysym );
            if( keysym == no_symbol || !stroke.has_value() )
            {
                return grab::fail( grab::ErrorCode::unsupported_character,
                                   "unsupported character " +
                                       codepoint_label( codepoint->value ) );
            }

            strokes.push_back( *stroke );
            utf8.remove_prefix( codepoint->byte_count );
        }

        return strokes;
    }

    std::optional<xkb_keysym_t>
    XkbKeymap::keysym_from_name( std::string_view name )
    {
        const std::string  name_storage{ name };
        const xkb_keysym_t keysym =
            xkb_keysym_from_name( name_storage.c_str(), XKB_KEYSYM_NO_FLAGS );
        if( keysym == no_symbol )
        {
            return std::nullopt;
        }
        return keysym;
    }

    void
    XkbKeymap::build_reverse_lookup()
    {
        const xkb_keycode_t min_keycode = xkb_keymap_min_keycode( keymap );
        const xkb_keycode_t max_keycode = xkb_keymap_max_keycode( keymap );

        xkb_keycode_t       keycode     = min_keycode;
        while( keycode <= max_keycode )
        {
            const xkb_level_index_t level_count =
                xkb_keymap_num_levels_for_key( keymap, keycode, default_layout );
            for( xkb_level_index_t level = base_level; level < level_count; ++level )
            {
                if( level <= shift_level )
                {
                    record_keysyms( keycode, level );
                }
            }

            if( keycode == max_keycode )
            {
                break;
            }
            ++keycode;
        }
    }

    void
    XkbKeymap::record_keysyms( xkb_keycode_t     keycode,
                               xkb_level_index_t level )
    {
        const xkb_keysym_t* syms      = nullptr;
        const int           sym_count = xkb_keymap_key_get_syms_by_level( keymap,
                                                                          keycode,
                                                                          default_layout,
                                                                          level,
                                                                          &syms );
        if( sym_count <= no_syms || syms == nullptr )
        {
            return;
        }

        const std::span<const xkb_keysym_t> symbols{
            syms,
            static_cast<std::size_t>( sym_count ),
        };
        for( const xkb_keysym_t keysym : symbols )
        {
            if( keysym == no_symbol || reverse.contains( keysym ) )
            {
                continue;
            }

            reverse.emplace( keysym,
                             KeyStroke{
                                 .keycode     = keycode,
                                 .needs_shift = level >= shift_level,
                             } );
        }
    }

}    // namespace grab::platform::x11
