#include "cli/input_command.hpp"
#include "grab/result.hpp"
#include "platform/x11/xkb_keymap.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <xkbcommon/xkbcommon.h>
// clang-format on

namespace
{

    namespace x11                                       = grab::platform::x11;

    constexpr std::string_view return_keysym_name       = "Return";
    constexpr std::string_view unknown_keysym_name      = "NoSuchGrabKeysym";
    constexpr std::string_view text                     = "aA!";
    constexpr std::size_t      expected_stroke_count    = 3U;
    constexpr std::size_t      lower_a_index            = 0U;
    constexpr std::size_t      upper_a_index            = 1U;
    constexpr std::size_t      exclamation_index        = 2U;
    constexpr char32_t         lower_a                  = U'a';
    constexpr char32_t         upper_a                  = U'A';
    constexpr char32_t         exclamation              = U'!';
    constexpr std::uint32_t    unsupported_codepoint    = 0X1FU;
    constexpr std::size_t      unsupported_byte_count   = 1U;
    constexpr std::string_view fraction_pair_text       = "0.06,0.235";
    constexpr std::string_view malformed_fraction_pair  = "0.06";
    constexpr double           expected_first_fraction  = 0.06;
    constexpr double           expected_second_fraction = 0.235;

    [[nodiscard]]
    xkb_keysym_t
    keysym_for_codepoint( char32_t codepoint ) noexcept
    {
        return xkb_utf32_to_keysym( static_cast<std::uint32_t>( codepoint ) );
    }

    void
    expect_round_trip( const x11::XkbKeymap&            keymap,
                       const x11::XkbKeymap::KeyStroke& stroke,
                       xkb_keysym_t                     expected )
    {
        auto actual = keymap.keysym_for_stroke( stroke );

        ASSERT_TRUE( actual.has_value() );
        EXPECT_EQ( *actual, expected );
    }

}    // namespace

TEST( XkbKeymap,
      ResolvesKnownKeysymNames )
{
    EXPECT_TRUE( x11::XkbKeymap::keysym_from_name( return_keysym_name ).has_value() );
    EXPECT_FALSE( x11::XkbKeymap::keysym_from_name( unknown_keysym_name ).has_value() );
}

TEST( XkbKeymap,
      MapsTextToShiftAwareStrokes )
{
    auto keymap = x11::XkbKeymap::from_default_names();
    ASSERT_TRUE( keymap.has_value() ) << keymap.error().message;

    auto strokes = keymap->strokes_for_text( text );
    ASSERT_TRUE( strokes.has_value() ) << strokes.error().message;
    ASSERT_EQ( strokes->size(), expected_stroke_count );

    EXPECT_FALSE( strokes->at( lower_a_index ).needs_shift );
    EXPECT_TRUE( strokes->at( upper_a_index ).needs_shift );
    EXPECT_TRUE( strokes->at( exclamation_index ).needs_shift );

    expect_round_trip( *keymap,
                       strokes->at( lower_a_index ),
                       keysym_for_codepoint( lower_a ) );
    expect_round_trip( *keymap,
                       strokes->at( upper_a_index ),
                       keysym_for_codepoint( upper_a ) );
    expect_round_trip( *keymap,
                       strokes->at( exclamation_index ),
                       keysym_for_codepoint( exclamation ) );
}

TEST( XkbKeymap,
      RejectsUnsupportedCodepoints )
{
    auto keymap = x11::XkbKeymap::from_default_names();
    ASSERT_TRUE( keymap.has_value() ) << keymap.error().message;

    const char unsupported = static_cast<char>( unsupported_codepoint );
    const auto unsupported_text =
        std::string_view{ &unsupported, unsupported_byte_count };

    auto strokes = keymap->strokes_for_text( unsupported_text );

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
