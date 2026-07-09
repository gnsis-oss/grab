#include "grab/keymap.hpp"
#include "grab/result.hpp"
#include "platform/x11/xcb_connection.hpp"
#include "platform/x11/xkb_keymap.hpp"

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

        constexpr xkb_layout_index_t kFirstLayout           = 0U;
        constexpr xkb_level_index_t  kLevelOne              = 0U;
        constexpr xkb_level_index_t  kLevelTwo              = 1U;
        constexpr xkb_level_index_t  kLevelThree            = 2U;
        constexpr xkb_level_index_t  kLevelFour             = 3U;
        constexpr xkb_level_index_t  kExpressibleLevelCount = 4U;
        constexpr xkb_keysym_t       kNoSymbol              = XKB_KEY_NoSymbol;
        constexpr std::uint32_t      kFallbackShiftKeycode  = 50U;
        constexpr std::uint32_t      kFallbackAltgrKeycode  = 92U;
        constexpr std::uint32_t      kHexDigitCount         = 16U;
        constexpr std::size_t        kMaxCodepointHexDigits = 8U;
        constexpr std::string_view   kHexDigits             = "0123456789ABCDEF";
        constexpr auto          kLayoutContextFlags  = XKB_CONTEXT_NO_ENVIRONMENT_NAMES;
        constexpr int           kInvalidDevice       = -1;
        constexpr std::uint8_t  kAsciiMax            = 0X7FU;
        constexpr std::uint8_t  kContinuationMin     = 0X80U;
        constexpr std::uint8_t  kContinuationMax     = 0XBFU;
        constexpr std::uint8_t  kTwoByteLeadMin      = 0XC2U;
        constexpr std::uint8_t  kTwoByteLeadMax      = 0XDFU;
        constexpr std::uint8_t  kThreeByteLeadMin    = 0XE0U;
        constexpr std::uint8_t  kThreeByteLeadMax    = 0XEFU;
        constexpr std::uint8_t  kFourByteLeadMin     = 0XF0U;
        constexpr std::uint8_t  kFourByteLeadMax     = 0XF4U;
        constexpr std::uint32_t kTwoByteCodepointMin = 0X80U;
        constexpr std::uint32_t kThreeByteCodepointMin = 0X8'00U;
        constexpr std::uint32_t kFourByteCodepointMin  = 0X1'00'00U;
        constexpr std::uint32_t kSurrogateMin          = 0XD8'00U;
        constexpr std::uint32_t kSurrogateMax          = 0XDF'FFU;
        constexpr std::uint32_t kMaxUnicodeCodepoint   = 0X10'FF'FFU;
        constexpr std::uint32_t kLowSixBitsMask        = 0X3FU;
        constexpr std::uint32_t kLowFourBitsMask       = 0X0FU;
        constexpr std::uint32_t kLowFiveBitsMask       = 0X1FU;
        constexpr std::uint32_t kLowThreeBitsMask      = 0X07U;
        constexpr unsigned int  kSixBitShift           = 6U;
        constexpr unsigned int  kTwelveBitShift        = 12U;
        constexpr unsigned int  kEighteenBitShift      = 18U;

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
            std::array<char, kMaxCodepointHexDigits> digits{};
            std::size_t                              cursor = digits.size();
            auto value = static_cast<std::uint32_t>( codepoint );
            if( value == 0U )
            {
                --cursor;
                digits.at( cursor ) = kHexDigits.at( 0U );
            }
            while( value != 0U && cursor > 0U )
            {
                --cursor;
                digits.at( cursor ) =
                    kHexDigits.at( static_cast<std::size_t>( value % kHexDigitCount ) );
                value /= kHexDigitCount;
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
            return byte >= kContinuationMin && byte <= kContinuationMax;
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
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "UTF-8 sequence is truncated at byte " +
                                       std::to_string( offset ) );
            }

            for( std::size_t index = 0; index < count; ++index )
            {
                if( !is_continuation( byte_at( utf8, offset + index ) ) )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
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
                kMaxUnicodeCodepoint ||
                ( codepoint >= kSurrogateMin && codepoint <= kSurrogateMax ) )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
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

            if( lead <= kAsciiMax )
            {
                ++offset;
                return static_cast<char32_t>( lead );
            }

            if( lead >= kTwoByteLeadMin && lead <= kTwoByteLeadMax )
            {
                auto continuation = require_continuation_bytes( utf8, offset + 1U, 1U );
                if( !continuation.has_value() )
                {
                    return std::unexpected( std::move( continuation.error() ) );
                }

                const std::uint32_t codepoint =
                    ( static_cast<std::uint32_t>( lead & kLowFiveBitsMask )
                      << kSixBitShift ) |
                    ( byte_at( utf8, offset + 1U ) & kLowSixBitsMask );
                offset += 2U;
                return validate_codepoint( codepoint, kTwoByteCodepointMin, start );
            }

            if( lead >= kThreeByteLeadMin && lead <= kThreeByteLeadMax )
            {
                auto continuation = require_continuation_bytes( utf8, offset + 1U, 2U );
                if( !continuation.has_value() )
                {
                    return std::unexpected( std::move( continuation.error() ) );
                }

                const std::uint32_t codepoint =
                    ( static_cast<std::uint32_t>( lead & kLowFourBitsMask )
                      << kTwelveBitShift ) |
                    ( static_cast<std::uint32_t>( byte_at( utf8, offset + 1U ) &
                                                  kLowSixBitsMask )
                      << kSixBitShift ) |
                    ( byte_at( utf8, offset + 2U ) & kLowSixBitsMask );
                offset += 3U;
                return validate_codepoint( codepoint, kThreeByteCodepointMin, start );
            }

            if( lead >= kFourByteLeadMin && lead <= kFourByteLeadMax )
            {
                auto continuation = require_continuation_bytes( utf8, offset + 1U, 3U );
                if( !continuation.has_value() )
                {
                    return std::unexpected( std::move( continuation.error() ) );
                }

                const std::uint32_t codepoint =
                    ( static_cast<std::uint32_t>( lead & kLowThreeBitsMask )
                      << kEighteenBitShift ) |
                    ( static_cast<std::uint32_t>( byte_at( utf8, offset + 1U ) &
                                                  kLowSixBitsMask )
                      << kTwelveBitShift ) |
                    ( static_cast<std::uint32_t>( byte_at( utf8, offset + 2U ) &
                                                  kLowSixBitsMask )
                      << kSixBitShift ) |
                    ( byte_at( utf8, offset + 3U ) & kLowSixBitsMask );
                offset += 4U;
                return validate_codepoint( codepoint, kFourByteCodepointMin, start );
            }

            return grab::fail( grab::ErrorCode::invalid_argument,
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
                .shift   = level == kLevelTwo || level == kLevelFour,
                .altgr   = level == kLevelThree || level == kLevelFour,
            };
        }

        [[nodiscard]]
        grab::Result<grab::Keystroke>
        unsupported_character( char32_t codepoint )
        {
            return grab::fail( grab::ErrorCode::unsupported_character,
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
            for( xkb_layout_index_t layout = kFirstLayout; layout < layouts; ++layout )
            {
                const xkb_level_index_t levels =
                    xkb_keymap_num_levels_for_key( keymap, keycode, layout );
                for( xkb_level_index_t level = kLevelOne; level < levels; ++level )
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
        std::uint32_t
        keycode_for_keysym( xkb_keymap*   keymap,
                            xkb_keysym_t  target,
                            std::uint32_t fallback )
        {
            if( keymap == nullptr )
            {
                return fallback;
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
            return fallback;
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
                        return grab::fail( grab::ErrorCode::internal_fault,
                                           "xkb keymap is not open" );
                    }

                    const auto keysym =
                        xkb_utf32_to_keysym( static_cast<std::uint32_t>( codepoint ) );
                    if( keysym == kNoSymbol )
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
                    if( keysym == kNoSymbol )
                    {
                        return std::nullopt;
                    }
                    return stroke_for_keysym( keysym );
                }

                [[nodiscard]]
                std::uint32_t
                shift_keycode() const override
                {
                    return keycode_for_keysym( keymap_.get(),
                                               XKB_KEY_Shift_L,
                                               kFallbackShiftKeycode );
                }

                [[nodiscard]]
                std::uint32_t
                altgr_keycode() const override
                {
                    return keycode_for_keysym( keymap_.get(),
                                               XKB_KEY_ISO_Level3_Shift,
                                               kFallbackAltgrKeycode );
                }

            private:

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
                        if( xkb_keymap_num_layouts_for_key( keymap_.get(), keycode ) ==
                            0U )
                        {
                            continue;
                        }

                        const xkb_level_index_t levels =
                            std::min( xkb_keymap_num_levels_for_key( keymap_.get(),
                                                                     keycode,
                                                                     kFirstLayout ),
                                      kExpressibleLevelCount );
                        for( xkb_level_index_t level = kLevelOne; level < levels;
                             ++level )
                        {
                            record_keysyms( keycode, level );
                        }
                    }
                }

                void
                record_keysyms( xkb_keycode_t     keycode,
                                xkb_level_index_t level )
                {
                    const xkb_keysym_t* symbols = nullptr;
                    const int           symbol_count =
                        xkb_keymap_key_get_syms_by_level( keymap_.get(),
                                                          keycode,
                                                          kFirstLayout,
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
                        if( keysym == kNoSymbol || reverse_.contains( keysym ) )
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
        auto context = take_context( xkb_context_new( kLayoutContextFlags ) );
        if( context == nullptr )
        {
            return grab::fail( grab::ErrorCode::internal_fault,
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
            return grab::fail( grab::ErrorCode::invalid_argument,
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
        if( device_id == kInvalidDevice )
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

        std::unique_ptr<grab::Keymap::Backend> backend =
            std::make_unique<XkbKeymap>( std::move( context ),
                                         std::move( keymap ),
                                         std::move( state ) );
        return grab::Keymap{ std::move( backend ) };
    }

}    // namespace grab::platform::x11
