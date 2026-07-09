#include "cli/input_command.hpp"
#include "grab/keymap.hpp"
#include "grab/result.hpp"
#include "platform/x11/xkb_keymap.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <xkbcommon/xkbcommon-names.h>
#include <xkbcommon/xkbcommon.h>
// clang-format on

namespace
{

    namespace x11                                         = grab::platform::x11;

    constexpr const char*        us_layout                = "us";
    constexpr std::string_view   return_keysym_name       = "Return";
    constexpr std::string_view   unknown_keysym_name      = "NoSuchGrabKeysym";
    constexpr std::string_view   text                     = "aA!";
    constexpr std::size_t        expected_stroke_count    = 3U;
    constexpr std::size_t        lower_a_index            = 0U;
    constexpr std::size_t        upper_a_index            = 1U;
    constexpr std::size_t        exclamation_index        = 2U;
    constexpr std::uint32_t      unsupported_codepoint    = 0X1FU;
    constexpr std::size_t        unsupported_byte_count   = 1U;
    constexpr std::string_view   fraction_pair_text       = "0.06,0.235";
    constexpr std::string_view   malformed_fraction_pair  = "0.06";
    constexpr double             expected_first_fraction  = 0.06;
    constexpr double             expected_second_fraction = 0.235;
    constexpr const char*        altgr_modifier_name      = "Mod5";
    constexpr xkb_mod_mask_t     no_modifier_mask         = 0U;
    constexpr xkb_layout_index_t default_layout           = 0U;
    constexpr std::size_t        null_terminator_size     = 1U;
    constexpr int                no_produced_bytes        = 0;
    constexpr auto               context_flags = XKB_CONTEXT_NO_ENVIRONMENT_NAMES;

    using XkbContext = std::unique_ptr<xkb_context, decltype( &xkb_context_unref )>;
    using XkbKeymap  = std::unique_ptr<xkb_keymap, decltype( &xkb_keymap_unref )>;
    using XkbState   = std::unique_ptr<xkb_state, decltype( &xkb_state_unref )>;

    struct TestXkb
    {
            XkbContext context;
            XkbKeymap  keymap;
    };

    [[nodiscard]]
    testing::AssertionResult
    open_test_keymap( TestXkb& output )
    {
        XkbContext context{ xkb_context_new( context_flags ), &xkb_context_unref };
        if( context == nullptr )
        {
            return testing::AssertionFailure() << "xkb_context_new failed";
        }

        const xkb_rule_names names{
            .rules   = nullptr,
            .model   = nullptr,
            .layout  = us_layout,
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
            return testing::AssertionFailure() << "xkb_keymap_new_from_names failed";
        }

        output.context = std::move( context );
        output.keymap  = std::move( keymap );
        return testing::AssertionSuccess();
    }

    [[nodiscard]]
    xkb_mod_mask_t
    modifier_bit( xkb_keymap& keymap,
                  const char* name ) noexcept
    {
        const xkb_mod_index_t index = xkb_keymap_mod_get_index( &keymap, name );
        constexpr auto        modifier_mask_digits =
            static_cast<xkb_mod_index_t>( std::numeric_limits<xkb_mod_mask_t>::digits );
        if( index == XKB_MOD_INVALID || index >= modifier_mask_digits )
        {
            return no_modifier_mask;
        }

        return static_cast<xkb_mod_mask_t>( 1U ) << index;
    }

    [[nodiscard]]
    xkb_mod_mask_t
    modifiers_for( xkb_keymap&            keymap,
                   const grab::Keystroke& stroke ) noexcept
    {
        xkb_mod_mask_t mask = no_modifier_mask;
        if( stroke.shift )
        {
            mask |= modifier_bit( keymap, XKB_MOD_NAME_SHIFT );
        }
        if( stroke.altgr )
        {
            mask |= modifier_bit( keymap, altgr_modifier_name );
        }
        return mask;
    }

    [[nodiscard]]
    testing::AssertionResult
    render_keystrokes( std::span<const grab::Keystroke> strokes,
                       std::string&                     output )
    {
        TestXkb oracle{
            .context = XkbContext{nullptr, &xkb_context_unref},
            .keymap  = XkbKeymap{nullptr,  &xkb_keymap_unref},
        };
        auto open_result = open_test_keymap( oracle );
        if( !open_result )
        {
            return open_result;
        }

        const XkbState state{ xkb_state_new( oracle.keymap.get() ), &xkb_state_unref };
        if( state == nullptr )
        {
            return testing::AssertionFailure() << "xkb_state_new failed";
        }

        output.clear();
        for( const auto& stroke : strokes )
        {
            static_cast<void>( xkb_state_update_mask( state.get(),
                                                      modifiers_for( *oracle.keymap,
                                                                     stroke ),
                                                      no_modifier_mask,
                                                      no_modifier_mask,
                                                      default_layout,
                                                      default_layout,
                                                      default_layout ) );

            const int required_bytes =
                xkb_state_key_get_utf8( state.get(), stroke.keycode, nullptr, 0U );
            if( required_bytes <= no_produced_bytes )
            {
                return testing::AssertionFailure() << "stroke produced no UTF-8 text";
            }

            std::string buffer( static_cast<std::size_t>( required_bytes ) +
                                    null_terminator_size,
                                '\0' );
            const int   written_bytes = xkb_state_key_get_utf8( state.get(),
                                                                stroke.keycode,
                                                                buffer.data(),
                                                                buffer.size() );
            if( written_bytes != required_bytes )
            {
                return testing::AssertionFailure()
                    << "xkb_state_key_get_utf8 changed size";
            }
            output.append( buffer.data(), static_cast<std::size_t>( written_bytes ) );
        }

        return testing::AssertionSuccess();
    }

}    // namespace

TEST( XkbKeymap,
      ResolvesKnownKeyNames )
{
    auto keymap = x11::make_keymap_from_layout( us_layout );
    ASSERT_TRUE( keymap.has_value() ) << keymap.error().message;

    EXPECT_TRUE( keymap->keystroke_for_key( return_keysym_name ).has_value() );
    EXPECT_FALSE( keymap->keystroke_for_key( unknown_keysym_name ).has_value() );
}

TEST( XkbKeymap,
      MapsTextToShiftAwareStrokes )
{
    auto keymap = x11::make_keymap_from_layout( us_layout );
    ASSERT_TRUE( keymap.has_value() ) << keymap.error().message;

    auto strokes = keymap->text_to_keystrokes( text );
    ASSERT_TRUE( strokes.has_value() ) << strokes.error().message;
    ASSERT_EQ( strokes->size(), expected_stroke_count );

    EXPECT_FALSE( strokes->at( lower_a_index ).shift );
    EXPECT_FALSE( strokes->at( lower_a_index ).altgr );
    EXPECT_TRUE( strokes->at( upper_a_index ).shift );
    EXPECT_FALSE( strokes->at( upper_a_index ).altgr );
    EXPECT_TRUE( strokes->at( exclamation_index ).shift );
    EXPECT_FALSE( strokes->at( exclamation_index ).altgr );

    std::string produced;
    ASSERT_TRUE( render_keystrokes( *strokes, produced ) );
    EXPECT_EQ( produced, text );
}

TEST( XkbKeymap,
      RejectsUnsupportedCodepoints )
{
    auto keymap = x11::make_keymap_from_layout( us_layout );
    ASSERT_TRUE( keymap.has_value() ) << keymap.error().message;

    const char unsupported = static_cast<char>( unsupported_codepoint );
    const auto unsupported_text =
        std::string_view{ &unsupported, unsupported_byte_count };

    auto strokes = keymap->text_to_keystrokes( unsupported_text );

    ASSERT_FALSE( strokes.has_value() );
    EXPECT_EQ( strokes.error().code, grab::ErrorCode::unsupported_character );
}

TEST( InputCommand,
      ParsesFractionPairs )
{
    auto parsed = grab::cli::parse_fraction_pair( fraction_pair_text );
    ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

    EXPECT_DOUBLE_EQ( parsed->first, expected_first_fraction );
    EXPECT_DOUBLE_EQ( parsed->second, expected_second_fraction );
}

TEST( InputCommand,
      RejectsMalformedFractionPairs )
{
    auto parsed = grab::cli::parse_fraction_pair( malformed_fraction_pair );

    ASSERT_FALSE( parsed.has_value() );
    EXPECT_EQ( parsed.error().code, grab::ErrorCode::invalid_argument );
}
