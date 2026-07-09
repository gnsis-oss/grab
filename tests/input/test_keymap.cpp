#include "grab/keymap.hpp"
#include "grab/result.hpp"
#include "platform/x11/xkb_keymap.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <cstddef>
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

    constexpr const char*        kUsLayout                = "us";
    constexpr char32_t           kLowercaseA              = U'a';
    constexpr char32_t           kUppercaseA              = U'A';
    constexpr char32_t           kUnmappableCodepoint     = U'\U00004E2D';
    constexpr std::string_view   kUnmappableCodepointName = "U+4E2D";
    constexpr std::string_view   kLowercaseAText          = "a";
    constexpr std::string_view   kUppercaseAText          = "A";
    constexpr std::string_view   kRoundTripText           = "Ab";
    constexpr std::size_t        kRoundTripKeystrokeCount = 2U;
    constexpr const char*        kAltgrModifierName       = "Mod5";
    constexpr xkb_mod_mask_t     kNoModifierMask          = 0U;
    constexpr xkb_layout_index_t kDefaultLayout           = 0U;
    constexpr std::size_t        kNullTerminatorSize      = 1U;
    constexpr std::size_t        kSingleKeystrokeCount    = 1U;
    constexpr int                kNoProducedBytes         = 0;
    constexpr auto               kContextFlags = XKB_CONTEXT_NO_ENVIRONMENT_NAMES;
    constexpr auto kUnsupportedCharacterCode   = grab::ErrorCode::unsupported_character;

    using XkbContext      = std::unique_ptr<xkb_context, decltype( &xkb_context_unref )>;
    using XkbKeymap       = std::unique_ptr<xkb_keymap, decltype( &xkb_keymap_unref )>;
    using XkbState        = std::unique_ptr<xkb_state, decltype( &xkb_state_unref )>;
    using SingleKeystroke = std::array<grab::Keystroke, kSingleKeystrokeCount>;

    struct TestXkb
    {
            XkbContext context;
            XkbKeymap  keymap;
    };

    [[nodiscard]]
    testing::AssertionResult
    open_test_keymap( TestXkb& output )
    {
        XkbContext context{ xkb_context_new( kContextFlags ), &xkb_context_unref };
        if( context == nullptr )
        {
            return testing::AssertionFailure() << "xkb_context_new failed";
        }

        const xkb_rule_names names{
            .rules   = nullptr,
            .model   = nullptr,
            .layout  = kUsLayout,
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
        constexpr auto        kModifierMaskDigits =
            static_cast<xkb_mod_index_t>( std::numeric_limits<xkb_mod_mask_t>::digits );
        if( index == XKB_MOD_INVALID || index >= kModifierMaskDigits )
        {
            return kNoModifierMask;
        }

        return static_cast<xkb_mod_mask_t>( 1U ) << index;
    }

    [[nodiscard]]
    xkb_mod_mask_t
    modifiers_for( xkb_keymap&            keymap,
                   const grab::Keystroke& keystroke ) noexcept
    {
        xkb_mod_mask_t mask = kNoModifierMask;
        if( keystroke.shift )
        {
            mask |= modifier_bit( keymap, XKB_MOD_NAME_SHIFT );
        }
        if( keystroke.altgr )
        {
            mask |= modifier_bit( keymap, kAltgrModifierName );
        }
        return mask;
    }

    [[nodiscard]]
    testing::AssertionResult
    render_keystrokes( std::span<const grab::Keystroke> keystrokes,
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
        for( const auto& keystroke : keystrokes )
        {
            static_cast<void>( xkb_state_update_mask( state.get(),
                                                      modifiers_for( *oracle.keymap,
                                                                     keystroke ),
                                                      kNoModifierMask,
                                                      kNoModifierMask,
                                                      kDefaultLayout,
                                                      kDefaultLayout,
                                                      kDefaultLayout ) );

            const int required_bytes =
                xkb_state_key_get_utf8( state.get(), keystroke.keycode, nullptr, 0U );
            if( required_bytes <= kNoProducedBytes )
            {
                return testing::AssertionFailure() << "keystroke produced no UTF-8 text";
            }

            std::string buffer( static_cast<std::size_t>( required_bytes ) +
                                    kNullTerminatorSize,
                                '\0' );
            const int   written_bytes = xkb_state_key_get_utf8( state.get(),
                                                                keystroke.keycode,
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

    [[nodiscard]]
    testing::AssertionResult
    render_keystroke( const grab::Keystroke& keystroke,
                      std::string&           output )
    {
        const SingleKeystroke keystrokes{ keystroke };
        return render_keystrokes( keystrokes, output );
    }

}    // namespace

TEST( Keymap,
      MapsLowercaseAscii )
{
    auto keymap = grab::platform::x11::make_keymap_from_layout( kUsLayout );
    ASSERT_TRUE( keymap.has_value() ) << keymap.error().message;

    auto keystroke = keymap->codepoint_to_keystroke( kLowercaseA );
    ASSERT_TRUE( keystroke.has_value() ) << keystroke.error().message;
    EXPECT_FALSE( keystroke->shift );
    EXPECT_FALSE( keystroke->altgr );
    EXPECT_NE( keymap->shift_keycode(), 0U );
    EXPECT_NE( keymap->altgr_keycode(), 0U );

    std::string produced;
    ASSERT_TRUE( render_keystroke( *keystroke, produced ) );
    EXPECT_EQ( produced, kLowercaseAText );
}

TEST( Keymap,
      UppercaseNeedsShift )
{
    auto keymap = grab::platform::x11::make_keymap_from_layout( kUsLayout );
    ASSERT_TRUE( keymap.has_value() ) << keymap.error().message;

    auto lowercase = keymap->codepoint_to_keystroke( kLowercaseA );
    ASSERT_TRUE( lowercase.has_value() ) << lowercase.error().message;
    auto uppercase = keymap->codepoint_to_keystroke( kUppercaseA );
    ASSERT_TRUE( uppercase.has_value() ) << uppercase.error().message;

    EXPECT_EQ( uppercase->keycode, lowercase->keycode );
    EXPECT_TRUE( uppercase->shift );
    EXPECT_FALSE( uppercase->altgr );

    std::string produced;
    ASSERT_TRUE( render_keystroke( *uppercase, produced ) );
    EXPECT_EQ( produced, kUppercaseAText );
}

TEST( Keymap,
      TextToKeystrokesRoundTrips )
{
    auto keymap = grab::platform::x11::make_keymap_from_layout( kUsLayout );
    ASSERT_TRUE( keymap.has_value() ) << keymap.error().message;

    auto keystrokes = keymap->text_to_keystrokes( kRoundTripText );
    ASSERT_TRUE( keystrokes.has_value() ) << keystrokes.error().message;
    ASSERT_EQ( keystrokes->size(), kRoundTripKeystrokeCount );

    std::string produced;
    ASSERT_TRUE( render_keystrokes( *keystrokes, produced ) );
    EXPECT_EQ( produced, kRoundTripText );
}

TEST( Keymap,
      UnmappableReturnsError )
{
    auto keymap = grab::platform::x11::make_keymap_from_layout( kUsLayout );
    ASSERT_TRUE( keymap.has_value() ) << keymap.error().message;

    auto result = keymap->codepoint_to_keystroke( kUnmappableCodepoint );
    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, kUnsupportedCharacterCode );
    EXPECT_NE( result.error().message.find( kUnmappableCodepointName ),
               std::string::npos );
}
