#include "drivers/desktop/x11/xcb_connection.hpp"
#include "drivers/desktop/x11/xkb_keymap.hpp"
#include "grab/keymap.hpp"
#include "grab/result.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
// clang-format off
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-x11.h>
#include <xkbcommon/xkbcommon.h>
// clang-format on

namespace grab::platform::x11
{

    namespace
    {

        constexpr xkb_layout_index_t firstLayout           = 0U;
        constexpr xkb_level_index_t  levelOne              = 0U;
        constexpr xkb_level_index_t  levelTwo              = 1U;
        constexpr xkb_level_index_t  levelThree            = 2U;
        constexpr xkb_level_index_t  levelFour             = 3U;
        constexpr xkb_level_index_t  expressibleLevelCount = 4U;
        constexpr xkb_keysym_t       noSymbol              = XKB_KEY_NoSymbol;
        constexpr std::uint32_t      hexDigitCount         = 16U;
        constexpr std::size_t        maxCodepointHexDigits = 8U;
        constexpr std::string_view   hexDigits             = "0123456789ABCDEF";
        constexpr auto          layoutContextFlags    = XKB_CONTEXT_NO_ENVIRONMENT_NAMES;
        constexpr int           invalidDevice         = -1;
        constexpr std::uint8_t  asciiMax              = 0X7FU;
        constexpr std::uint8_t  continuationMin       = 0X80U;
        constexpr std::uint8_t  continuationMax       = 0XBFU;
        constexpr std::uint8_t  twoByteLeadMin        = 0XC2U;
        constexpr std::uint8_t  twoByteLeadMax        = 0XDFU;
        constexpr std::uint8_t  threeByteLeadMin      = 0XE0U;
        constexpr std::uint8_t  threeByteLeadMax      = 0XEFU;
        constexpr std::uint8_t  fourByteLeadMin       = 0XF0U;
        constexpr std::uint8_t  fourByteLeadMax       = 0XF4U;
        constexpr std::uint32_t twoByteCodepointMin   = 0X80U;
        constexpr std::uint32_t threeByteCodepointMin = 0X8'00U;
        constexpr std::uint32_t fourByteCodepointMin  = 0X1'00'00U;
        constexpr std::uint32_t surrogateMin          = 0XD8'00U;
        constexpr std::uint32_t surrogateMax          = 0XDF'FFU;
        constexpr std::uint32_t maxUnicodeCodepoint   = 0X10'FF'FFU;
        constexpr std::uint32_t lowSixBitsMask        = 0X3FU;
        constexpr std::uint32_t lowFourBitsMask       = 0X0FU;
        constexpr std::uint32_t lowFiveBitsMask       = 0X1FU;
        constexpr std::uint32_t lowThreeBitsMask      = 0X07U;
        constexpr unsigned int  sixBitShift           = 6U;
        constexpr unsigned int  twelveBitShift        = 12U;
        constexpr unsigned int  eighteenBitShift      = 18U;

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
        std::string
        codepoint_name( char32_t codepoint )
        {
            std::array<char, maxCodepointHexDigits> digits{};
            std::size_t                             cursor = digits.size();
            auto value = static_cast<std::uint32_t>( codepoint );
            if( value == 0U )
            {
                --cursor;
                digits.at( cursor ) = hexDigits.at( 0U );
            }
            while( value != 0U && cursor > 0U )
            {
                --cursor;
                digits.at( cursor ) =
                    hexDigits.at( static_cast<std::size_t>( value % hexDigitCount ) );
                value /= hexDigitCount;
            }

            std::string name{ "U+" };
            for( std::size_t index = cursor; index < digits.size(); ++index )
            {
                name.push_back( digits.at( index ) );
            }
            return name;
        }

        [[nodiscard]]
        bool
        is_continuation( std::uint8_t byte ) noexcept
        {
            return byte >= continuationMin && byte <= continuationMax;
        }

        [[nodiscard]]
        std::uint8_t
        byte_at( std::string_view utf8,
                 std::size_t      offset )
        {
            return static_cast<std::uint8_t>(
                static_cast<unsigned char>( utf8.at( offset ) )
            );
        }

        [[nodiscard]]
        grab::Result<void>
        require_continuation_bytes( std::string_view utf8,
                                    std::size_t      offset,
                                    std::size_t      count )
        {
            if( utf8.size() - offset < count )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "UTF-8 sequence is truncated at byte " +
                                       std::to_string( offset ) );
            }

            for( std::size_t index = 0; index < count; ++index )
            {
                if( !is_continuation( byte_at( utf8, offset + index ) ) )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "UTF-8 continuation byte is invalid at byte " +
                                           std::to_string( offset + index ) );
                }
            }

            return {};
        }

        [[nodiscard]]
        grab::Result<char32_t>
        validate_codepoint( std::uint32_t codepoint,
                            std::uint32_t minimum,
                            std::size_t   offset )
        {
            if( codepoint <
                minimum ||
                codepoint >
                maxUnicodeCodepoint ||
                ( codepoint >= surrogateMin && codepoint <= surrogateMax ) )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "UTF-8 codepoint is invalid at byte " +
                                       std::to_string( offset ) );
            }

            return static_cast<char32_t>( codepoint );
        }

        [[nodiscard]]
        grab::Result<char32_t>
        decode_utf8_codepoint( std::string_view utf8,
                               std::size_t&     offset )
        {
            const std::size_t  start = offset;
            const std::uint8_t lead  = byte_at( utf8, offset );

            if( lead <= asciiMax )
            {
                ++offset;
                return static_cast<char32_t>( lead );
            }

            if( lead >= twoByteLeadMin && lead <= twoByteLeadMax )
            {
                auto continuation = require_continuation_bytes( utf8, offset + 1U, 1U );
                if( !continuation.has_value() )
                {
                    return std::unexpected( std::move( continuation.error() ) );
                }

                const std::uint32_t codepoint =
                    ( static_cast<std::uint32_t>( lead & lowFiveBitsMask )
                      << sixBitShift ) |
                    ( byte_at( utf8, offset + 1U ) & lowSixBitsMask );
                offset += 2U;
                return validate_codepoint( codepoint, twoByteCodepointMin, start );
            }

            if( lead >= threeByteLeadMin && lead <= threeByteLeadMax )
            {
                auto continuation = require_continuation_bytes( utf8, offset + 1U, 2U );
                if( !continuation.has_value() )
                {
                    return std::unexpected( std::move( continuation.error() ) );
                }

                const std::uint32_t codepoint =
                    ( static_cast<std::uint32_t>( lead & lowFourBitsMask )
                      << twelveBitShift ) |
                    ( static_cast<std::uint32_t>( byte_at( utf8, offset + 1U ) &
                                                  lowSixBitsMask )
                      << sixBitShift ) |
                    ( byte_at( utf8, offset + 2U ) & lowSixBitsMask );
                offset += 3U;
                return validate_codepoint( codepoint, threeByteCodepointMin, start );
            }

            if( lead >= fourByteLeadMin && lead <= fourByteLeadMax )
            {
                auto continuation = require_continuation_bytes( utf8, offset + 1U, 3U );
                if( !continuation.has_value() )
                {
                    return std::unexpected( std::move( continuation.error() ) );
                }

                const std::uint32_t codepoint =
                    ( static_cast<std::uint32_t>( lead & lowThreeBitsMask )
                      << eighteenBitShift ) |
                    ( static_cast<std::uint32_t>( byte_at( utf8, offset + 1U ) &
                                                  lowSixBitsMask )
                      << twelveBitShift ) |
                    ( static_cast<std::uint32_t>( byte_at( utf8, offset + 2U ) &
                                                  lowSixBitsMask )
                      << sixBitShift ) |
                    ( byte_at( utf8, offset + 3U ) & lowSixBitsMask );
                offset += 4U;
                return validate_codepoint( codepoint, fourByteCodepointMin, start );
            }

            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "UTF-8 leading byte is invalid at byte " +
                                   std::to_string( start ) );
        }

        [[nodiscard]]
        grab::Keystroke
        keystroke_for_level( xkb_keycode_t     keycode,
                             xkb_level_index_t level ) noexcept
        {
            return grab::Keystroke{
                .keycode = static_cast<std::uint32_t>( keycode ),
                .shift   = level == levelTwo || level == levelFour,
                .altgr   = level == levelThree || level == levelFour,
            };
        }

        [[nodiscard]]
        grab::Result<grab::Keystroke>
        unsupported_character( char32_t codepoint )
        {
            return grab::fail( grab::ErrorCode::UnsupportedCharacter,
                               "unsupported character " + codepoint_name( codepoint ) );
        }

        [[nodiscard]]
        bool
        key_has_keysym( xkb_keymap*   keymap,
                        xkb_keycode_t keycode,
                        xkb_keysym_t  target )
        {
            const xkb_layout_index_t layouts =
                xkb_keymap_num_layouts_for_key( keymap, keycode );
            for( xkb_layout_index_t layout = firstLayout; layout < layouts; ++layout )
            {
                const xkb_level_index_t levels =
                    xkb_keymap_num_levels_for_key( keymap, keycode, layout );
                for( xkb_level_index_t level = levelOne; level < levels; ++level )
                {
                    const xkb_keysym_t* symbols = nullptr;
                    const int           symbol_count =
                        xkb_keymap_key_get_syms_by_level( keymap,
                                                          keycode,
                                                          layout,
                                                          level,
                                                          &symbols );
                    if( symbol_count <= 0 || symbols == nullptr )
                    {
                        continue;
                    }

                    const std::span<const xkb_keysym_t> keysyms{
                        symbols,
                        static_cast<std::size_t>( symbol_count )
                    };
                    if( std::ranges::find( keysyms, target ) != keysyms.end() )
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        [[nodiscard]]
        std::optional<std::uint32_t>
        keycode_for_keysym( xkb_keymap*  keymap,
                            xkb_keysym_t target )
        {
            if( keymap == nullptr )
            {
                return std::nullopt;
            }

            const xkb_keycode_t min_keycode = xkb_keymap_min_keycode( keymap );
            const xkb_keycode_t max_keycode = xkb_keymap_max_keycode( keymap );
            for( xkb_keycode_t keycode = min_keycode; keycode <= max_keycode; ++keycode )
            {
                if( keycode > std::numeric_limits<std::uint8_t>::max() )
                {
                    continue;
                }
                if( key_has_keysym( keymap, keycode, target ) )
                {
                    return static_cast<std::uint32_t>( keycode );
                }
            }
            return std::nullopt;
        }

        class XkbKeymap final : public grab::Keymap::Backend
        {
            public:

                XkbKeymap( XkbContextHandle context,
                           XkbKeymapHandle  keymap,
                           XkbStateHandle   state ) :
                    context_( std::move( context ) ),
                    keymap_( std::move( keymap ) ),
                    state_( std::move( state ) )
                {
                    build_reverse_lookup();
                }

                [[nodiscard]]
                grab::Result<std::vector<grab::Keystroke>>
                text_to_keystrokes( std::string_view utf8 ) const override
                {
                    std::vector<grab::Keystroke> keystrokes;
                    keystrokes.reserve( utf8.size() );

                    std::size_t offset = 0U;
                    while( offset < utf8.size() )
                    {
                        auto codepoint = decode_utf8_codepoint( utf8, offset );
                        if( !codepoint.has_value() )
                        {
                            return std::unexpected( std::move( codepoint.error() ) );
                        }

                        auto keystroke = codepoint_to_keystroke( *codepoint );
                        if( !keystroke.has_value() )
                        {
                            return std::unexpected( std::move( keystroke.error() ) );
                        }
                        keystrokes.push_back( *keystroke );
                    }

                    return keystrokes;
                }

                [[nodiscard]]
                grab::Result<grab::Keystroke>
                codepoint_to_keystroke( char32_t codepoint ) const override
                {
                    if( keymap_ == nullptr )
                    {
                        return grab::fail( grab::ErrorCode::InternalFault,
                                           "xkb keymap is not open" );
                    }

                    const auto keysym =
                        xkb_utf32_to_keysym( static_cast<std::uint32_t>( codepoint ) );
                    if( keysym == noSymbol )
                    {
                        return unsupported_character( codepoint );
                    }

                    auto keystroke = stroke_for_keysym( keysym );
                    if( !keystroke.has_value() )
                    {
                        return unsupported_character( codepoint );
                    }
                    return *keystroke;
                }

                [[nodiscard]]
                std::optional<grab::Keystroke>
                keystroke_for_key( std::string_view name ) const override
                {
                    const std::string  name_storage{ name };
                    const xkb_keysym_t keysym =
                        xkb_keysym_from_name( name_storage.c_str(),
                                              XKB_KEYSYM_NO_FLAGS );
                    if( keysym == noSymbol )
                    {
                        return std::nullopt;
                    }
                    return stroke_for_keysym( keysym );
                }

                [[nodiscard]]
                std::uint32_t
                shift_keycode() const override
                {
                    auto keycode = keycode_for_keysym( keymap_.get(), XKB_KEY_Shift_L );
                    if( keycode.has_value() )
                    {
                        return *keycode;
                    }
                    return keycode_for_keysym( keymap_.get(), XKB_KEY_Shift_R )
                        .value_or( 0U );
                }

                [[nodiscard]]
                std::uint32_t
                altgr_keycode() const override
                {
                    return keycode_for_keysym( keymap_.get(), XKB_KEY_ISO_Level3_Shift )
                        .value_or( 0U );
                }

            private:

                [[nodiscard]]
                xkb_layout_index_t
                layout_for_key( xkb_keycode_t keycode ) const noexcept
                {
                    if( state_ != nullptr )
                    {
                        const xkb_layout_index_t layout =
                            xkb_state_key_get_layout( state_.get(), keycode );
                        if( layout != XKB_LAYOUT_INVALID )
                        {
                            return layout;
                        }
                    }
                    return firstLayout;
                }

                [[nodiscard]]
                std::optional<grab::Keystroke>
                stroke_for_keysym( xkb_keysym_t keysym ) const
                {
                    const auto found = reverse_.find( keysym );
                    if( found == reverse_.end() )
                    {
                        return std::nullopt;
                    }
                    return ( *found ).second;
                }

                void
                build_reverse_lookup()
                {
                    if( keymap_ == nullptr )
                    {
                        return;
                    }

                    const xkb_keycode_t min_keycode =
                        xkb_keymap_min_keycode( keymap_.get() );
                    const xkb_keycode_t max_keycode =
                        xkb_keymap_max_keycode( keymap_.get() );
                    for( xkb_keycode_t keycode = min_keycode; keycode <= max_keycode;
                         ++keycode )
                    {
                        const xkb_layout_index_t layout = layout_for_key( keycode );
                        if( layout ==
                            XKB_LAYOUT_INVALID ||
                            layout >=
                            xkb_keymap_num_layouts_for_key( keymap_.get(), keycode ) )
                        {
                            continue;
                        }

                        const xkb_level_index_t levels =
                            std::min( xkb_keymap_num_levels_for_key( keymap_.get(),
                                                                     keycode,
                                                                     layout ),
                                      expressibleLevelCount );
                        for( xkb_level_index_t level = levelOne; level < levels;
                             ++level )
                        {
                            record_keysyms( keycode, layout, level );
                        }
                    }
                }

                void
                record_keysyms( xkb_keycode_t      keycode,
                                xkb_layout_index_t layout,
                                xkb_level_index_t  level )
                {
                    const xkb_keysym_t* symbols = nullptr;
                    const int           symbol_count =
                        xkb_keymap_key_get_syms_by_level( keymap_.get(),
                                                          keycode,
                                                          layout,
                                                          level,
                                                          &symbols );
                    if( symbol_count <= 0 || symbols == nullptr )
                    {
                        return;
                    }

                    const std::span<const xkb_keysym_t> keysyms{
                        symbols,
                        static_cast<std::size_t>( symbol_count )
                    };
                    for( const xkb_keysym_t keysym : keysyms )
                    {
                        if( keysym == noSymbol || reverse_.contains( keysym ) )
                        {
                            continue;
                        }

                        reverse_.emplace( keysym,
                                          keystroke_for_level( keycode, level ) );
                    }
                }

                XkbContextHandle context_{ nullptr, &xkb_context_unref };
                XkbKeymapHandle  keymap_{ nullptr, &xkb_keymap_unref };
                XkbStateHandle   state_{ nullptr, &xkb_state_unref };
                std::unordered_map<xkb_keysym_t, grab::Keystroke> reverse_;
        };

    }    // namespace

    grab::Result<grab::Keymap>
    make_keymap_from_layout( std::string_view layout )
    {
        auto context = take_context( xkb_context_new( layoutContextFlags ) );
        if( context == nullptr )
        {
            return grab::fail( grab::ErrorCode::InternalFault,
                               "xkb context allocation failed" );
        }

        const std::string    layout_name{ layout };
        const xkb_rule_names names{
            .rules   = nullptr,
            .model   = nullptr,
            .layout  = layout_name.c_str(),
            .variant = nullptr,
            .options = nullptr,
        };

        auto keymap =
            take_keymap( xkb_keymap_new_from_names( context.get(),
                                                    &names,
                                                    XKB_KEYMAP_COMPILE_NO_FLAGS ) );
        if( keymap == nullptr )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "xkb keymap compile failed for layout " + layout_name );
        }

        std::unique_ptr<grab::Keymap::Backend> backend =
            std::make_unique<XkbKeymap>( std::move( context ),
                                         std::move( keymap ),
                                         take_state( nullptr ) );
        return grab::Keymap{ std::move( backend ) };
    }

    grab::Result<grab::Keymap>
    make_keymap_from_connection( const XcbConnection& conn )
    {
        auto context = take_context( xkb_context_new( XKB_CONTEXT_NO_FLAGS ) );
        if( context == nullptr )
        {
            return grab::fail( grab::ErrorCode::InternalFault,
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
            return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                               "XKB extension is unavailable on the X display" );
        }

        const int device_id = xkb_x11_get_core_keyboard_device_id( conn.get() );
        if( device_id == invalidDevice )
        {
            return grab::fail( grab::ErrorCode::DeviceInaccessible,
                               "failed to resolve the XKB core keyboard device" );
        }

        auto keymap =
            take_keymap( xkb_x11_keymap_new_from_device( context.get(),
                                                         conn.get(),
                                                         device_id,
                                                         XKB_KEYMAP_COMPILE_NO_FLAGS ) );
        if( keymap == nullptr )
        {
            return grab::fail( grab::ErrorCode::ProviderFailed,
                               "failed to load the X server keymap" );
        }

        auto state = take_state(
            xkb_x11_state_new_from_device( keymap.get(), conn.get(), device_id )
        );
        if( state == nullptr )
        {
            return grab::fail( grab::ErrorCode::ProviderFailed,
                               "failed to load the X server keyboard state" );
        }

        std::unique_ptr<grab::Keymap::Backend> backend =
            std::make_unique<XkbKeymap>( std::move( context ),
                                         std::move( keymap ),
                                         std::move( state ) );
        return grab::Keymap{ std::move( backend ) };
    }

}    // namespace grab::platform::x11
