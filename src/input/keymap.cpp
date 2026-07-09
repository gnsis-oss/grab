#include "grab/result.hpp"
#include "input/keymap.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format off
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>
// clang-format on

namespace grab::input
{

    namespace
    {

        constexpr xkb_layout_index_t kFirstLayout           = 0U;
        constexpr xkb_level_index_t  kLevelOne              = 0U;
        constexpr xkb_level_index_t  kLevelTwo              = 1U;
        constexpr xkb_level_index_t  kLevelThree            = 2U;
        constexpr xkb_level_index_t  kLevelFour             = 3U;
        constexpr xkb_level_index_t  kExpressibleLevelCount = 4U;
        constexpr std::uint8_t       kFallbackShiftKeycode  = 50U;
        constexpr std::uint8_t       kFallbackLevel3Keycode = 92U;
        constexpr std::uint32_t      kHexDigitCount         = 16U;
        constexpr std::size_t        kMaxCodepointHexDigits = 8U;
        constexpr std::string_view   kHexDigits             = "0123456789ABCDEF";
        constexpr auto               kContextFlags    = XKB_CONTEXT_NO_ENVIRONMENT_NAMES;
        constexpr std::uint8_t       kAsciiMax        = 0X7FU;
        constexpr std::uint8_t       kContinuationMin = 0X80U;
        constexpr std::uint8_t       kContinuationMax = 0XBFU;
        constexpr std::uint8_t       kTwoByteLeadMin  = 0XC2U;
        constexpr std::uint8_t       kTwoByteLeadMax  = 0XDFU;
        constexpr std::uint8_t       kThreeByteLeadMin      = 0XE0U;
        constexpr std::uint8_t       kThreeByteLeadMax      = 0XEFU;
        constexpr std::uint8_t       kFourByteLeadMin       = 0XF0U;
        constexpr std::uint8_t       kFourByteLeadMax       = 0XF4U;
        constexpr std::uint32_t      kTwoByteCodepointMin   = 0X80U;
        constexpr std::uint32_t      kThreeByteCodepointMin = 0X8'00U;
        constexpr std::uint32_t      kFourByteCodepointMin  = 0X1'00'00U;
        constexpr std::uint32_t      kSurrogateMin          = 0XD8'00U;
        constexpr std::uint32_t      kSurrogateMax          = 0XDF'FFU;
        constexpr std::uint32_t      kMaxUnicodeCodepoint   = 0X10'FF'FFU;
        constexpr std::uint32_t      kLowSixBitsMask        = 0X3FU;
        constexpr std::uint32_t      kLowFourBitsMask       = 0X0FU;
        constexpr std::uint32_t      kLowFiveBitsMask       = 0X1FU;
        constexpr std::uint32_t      kLowThreeBitsMask      = 0X07U;
        constexpr unsigned int       kSixBitShift           = 6U;
        constexpr unsigned int       kTwelveBitShift        = 12U;
        constexpr unsigned int       kEighteenBitShift      = 18U;

        using XkbContext = std::unique_ptr<xkb_context, decltype( &xkb_context_unref )>;
        using XkbKeymap  = std::unique_ptr<xkb_keymap, decltype( &xkb_keymap_unref )>;

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
        Keystroke
        keystroke_for_level( xkb_keycode_t     keycode,
                             xkb_level_index_t level ) noexcept
        {
            return Keystroke{
                .keycode = static_cast<std::uint8_t>( keycode ),
                .shift   = level == kLevelTwo || level == kLevelFour,
                .level3  = level == kLevelThree || level == kLevelFour,
            };
        }

        [[nodiscard]]
        grab::Result<Keystroke>
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
        std::uint8_t
        keycode_for_keysym( xkb_keymap*  keymap,
                            xkb_keysym_t target,
                            std::uint8_t fallback )
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
                    return static_cast<std::uint8_t>( keycode );
                }
            }
            return fallback;
        }

    }    // namespace

    Keymap::Keymap( xkb_context* context,
                    xkb_keymap*  keymap ) noexcept :
        context_( context ),
        keymap_( keymap )
    {
    }

    Keymap::~Keymap()
    {
        if( keymap_ != nullptr )
        {
            xkb_keymap_unref( keymap_ );
        }
        if( context_ != nullptr )
        {
            xkb_context_unref( context_ );
        }
    }

    Keymap::Keymap( Keymap&& other ) noexcept :
        context_( std::exchange( other.context_,
                                 nullptr ) ),
        keymap_( std::exchange( other.keymap_,
                                nullptr ) )
    {
    }

    Keymap&
    Keymap::operator=( Keymap&& other ) noexcept
    {
        if( this != &other )
        {
            if( keymap_ != nullptr )
            {
                xkb_keymap_unref( keymap_ );
            }
            if( context_ != nullptr )
            {
                xkb_context_unref( context_ );
            }
            context_ = std::exchange( other.context_, nullptr );
            keymap_  = std::exchange( other.keymap_, nullptr );
        }
        return *this;
    }

    grab::Result<Keymap>
    Keymap::open_layout( std::string_view layout )
    {
        XkbContext context{ xkb_context_new( kContextFlags ), &xkb_context_unref };
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

        XkbKeymap keymap{
            xkb_keymap_new_from_names( context.get(),
                                       &names,
                                       XKB_KEYMAP_COMPILE_NO_FLAGS ),
            &xkb_keymap_unref
        };
        if( keymap == nullptr )
        {
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "xkb keymap compile failed for layout " + layout_name );
        }

        return Keymap{ context.release(), keymap.release() };
    }

    grab::Result<std::vector<Keystroke>>
    Keymap::text_to_keystrokes( std::string_view utf8 ) const
    {
        std::vector<Keystroke> keystrokes;
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

    grab::Result<Keystroke>
    Keymap::codepoint_to_keystroke( char32_t codepoint ) const
    {
        if( keymap_ == nullptr )
        {
            return grab::fail( grab::ErrorCode::internal_fault,
                               "xkb keymap is not open" );
        }

        const auto keysym =
            xkb_utf32_to_keysym( static_cast<std::uint32_t>( codepoint ) );
        if( keysym == XKB_KEY_NoSymbol )
        {
            return unsupported_character( codepoint );
        }

        const xkb_keycode_t min_keycode = xkb_keymap_min_keycode( keymap_ );
        const xkb_keycode_t max_keycode = xkb_keymap_max_keycode( keymap_ );
        for( xkb_keycode_t keycode = min_keycode; keycode <= max_keycode; ++keycode )
        {
            if( keycode >
                std::numeric_limits<std::uint8_t>::max() ||
                xkb_keymap_num_layouts_for_key( keymap_, keycode ) == 0U )
            {
                continue;
            }

            const xkb_level_index_t levels = std::min(
                xkb_keymap_num_levels_for_key( keymap_, keycode, kFirstLayout ),
                kExpressibleLevelCount
            );
            for( xkb_level_index_t level = kLevelOne; level < levels; ++level )
            {
                const xkb_keysym_t* symbols = nullptr;
                const int symbol_count = xkb_keymap_key_get_syms_by_level( keymap_,
                                                                           keycode,
                                                                           kFirstLayout,
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
                if( std::ranges::find( keysyms, keysym ) != keysyms.end() )
                {
                    return keystroke_for_level( keycode, level );
                }
            }
        }

        return unsupported_character( codepoint );
    }

    std::uint8_t
    Keymap::shift_keycode() const
    {
        return keycode_for_keysym( keymap_, XKB_KEY_Shift_L, kFallbackShiftKeycode );
    }

    std::uint8_t
    Keymap::level3_keycode() const
    {
        return keycode_for_keysym( keymap_,
                                   XKB_KEY_ISO_Level3_Shift,
                                   kFallbackLevel3Keycode );
    }

}    // namespace grab::input
