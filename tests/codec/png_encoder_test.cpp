#include "codec/png/png_encoder.hpp"
#include "grab/image.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    constexpr std::string_view fixture_root     = GRAB_CODEC_FIXTURES_DIR;
    constexpr std::string_view manifest_name    = "manifest.txt";
    constexpr std::string_view raw_extension    = ".raw";
    constexpr std::string_view png_extension    = ".png";
    constexpr std::uint32_t    empty_dimension  = 0U;
    constexpr std::size_t      rgb24_stride     = 3U;
    constexpr std::size_t      bgr0_stride      = 4U;
    constexpr std::ptrdiff_t   single_channel   = 1;
    constexpr std::uint32_t    bgr0_test_width  = 2U;
    constexpr std::uint32_t    bgr0_test_height = 2U;
    constexpr std::uint8_t     channel_min      = 0U;
    constexpr std::uint8_t     channel_max      = 255U;
    constexpr std::uint8_t     bgr0_pad_byte    = 0U;

    struct FixtureEntry
    {
            std::string   name;
            std::uint32_t width  = empty_dimension;
            std::uint32_t height = empty_dimension;
    };

    [[nodiscard]]
    std::filesystem::path
    fixtures_dir()
    {
        return std::filesystem::path{ std::string{ fixture_root } };
    }

    [[nodiscard]]
    std::filesystem::path
    fixture_path( std::string_view name )
    {
        return fixtures_dir() / std::string{ name };
    }

    [[nodiscard]]
    std::filesystem::path
    fixture_path( std::string_view name,
                  std::string_view extension )
    {
        std::string filename{ name };
        filename += extension;
        return fixture_path( filename );
    }

    [[nodiscard]]
    std::vector<std::uint8_t>
    read_binary_file( const std::filesystem::path& path )
    {
        std::ifstream input( path, std::ios::binary );
        EXPECT_TRUE( input.is_open() ) << "failed to open " << path;
        const std::string contents{
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{}
        };
        return std::vector<std::uint8_t>{ contents.begin(), contents.end() };
    }

    [[nodiscard]]
    std::vector<FixtureEntry>
    read_manifest()
    {
        std::ifstream input( fixture_path( manifest_name ) );
        EXPECT_TRUE( input.is_open() ) << "failed to open manifest";

        std::vector<FixtureEntry> entries;
        std::string               line;
        while( std::getline( input, line ) )
        {
            std::istringstream line_stream( line );
            FixtureEntry       entry;
            line_stream >> entry.name >> entry.width >> entry.height;
            EXPECT_FALSE( entry.name.empty() ) << "invalid manifest line: " << line;
            entries.push_back( std::move( entry ) );
        }

        return entries;
    }

    [[nodiscard]]
    std::size_t
    first_differing_offset( std::span<const std::uint8_t> lhs,
                            std::span<const std::uint8_t> rhs )
    {
        const auto [lhs_mismatch, rhs_mismatch] = std::ranges::mismatch( lhs, rhs );
        if( lhs_mismatch == lhs.end() || rhs_mismatch == rhs.end() )
        {
            return std::min( lhs.size(), rhs.size() );
        }
        return static_cast<std::size_t>( std::ranges::distance( lhs.begin(),
                                                                lhs_mismatch ) );
    }

    void
    expect_equal_bytes( std::span<const std::uint8_t> lhs,
                        std::span<const std::uint8_t> rhs )
    {
        const std::size_t offset = first_differing_offset( lhs, rhs );
        ASSERT_EQ( lhs.size(), rhs.size() ) << "first differing offset: " << offset;
        EXPECT_TRUE( std::ranges::equal( lhs, rhs ) )
            << "first differing offset: " << offset;
    }

    [[nodiscard]]
    std::vector<std::uint8_t>
    bgr0_from_rgb24( std::span<const std::uint8_t> rgb )
    {
        std::vector<std::uint8_t> bgr0;
        bgr0.reserve( ( rgb.size() / rgb24_stride ) * bgr0_stride );

        auto pixel = rgb.begin();
        while( pixel != rgb.end() )
        {
            const std::uint8_t red = *pixel;
            std::advance( pixel, single_channel );
            const std::uint8_t green = *pixel;
            std::advance( pixel, single_channel );
            const std::uint8_t blue = *pixel;
            std::advance( pixel, single_channel );

            bgr0.push_back( blue );
            bgr0.push_back( green );
            bgr0.push_back( red );
            bgr0.push_back( bgr0_pad_byte );
        }

        return bgr0;
    }

}    // namespace

TEST( PngEncoder,
      MatchesFfmpegFixtureBytes )
{
    const auto entries = read_manifest();
    ASSERT_FALSE( entries.empty() );

    for( const auto& entry : entries )
    {
        SCOPED_TRACE( entry.name );

        const auto raw = read_binary_file( fixture_path( entry.name, raw_extension ) );
        const auto golden =
            read_binary_file( fixture_path( entry.name, png_extension ) );

        const grab::ImageView image{
            .data   = raw.data(),
            .width  = entry.width,
            .height = entry.height,
            .stride = static_cast<std::size_t>( entry.width ) * rgb24_stride,
            .format = grab::PixelFormat::rgb24,
        };

        const auto encoded = grab::codec::encode_png( image );
        ASSERT_TRUE( encoded.has_value() ) << encoded.error().message;
        expect_equal_bytes( *encoded, golden );
    }
}

TEST( PngEncoder,
      Bgr0EncodingMatchesEquivalentRgb24Encoding )
{
    const std::vector<std::uint8_t> rgb{
        channel_max,
        channel_min,
        channel_min,
        channel_min,
        channel_max,
        channel_min,
        channel_min,
        channel_min,
        channel_max,
        channel_max,
        channel_max,
        channel_max,
    };
    const auto            bgr0 = bgr0_from_rgb24( rgb );

    const grab::ImageView rgb_image{
        .data   = rgb.data(),
        .width  = bgr0_test_width,
        .height = bgr0_test_height,
        .stride = static_cast<std::size_t>( bgr0_test_width ) * rgb24_stride,
        .format = grab::PixelFormat::rgb24,
    };
    const grab::ImageView bgr0_image{
        .data   = bgr0.data(),
        .width  = bgr0_test_width,
        .height = bgr0_test_height,
        .stride = static_cast<std::size_t>( bgr0_test_width ) * bgr0_stride,
        .format = grab::PixelFormat::bgr0,
    };

    const auto rgb_png  = grab::codec::encode_png( rgb_image );
    const auto bgr0_png = grab::codec::encode_png( bgr0_image );

    ASSERT_TRUE( rgb_png.has_value() ) << rgb_png.error().message;
    ASSERT_TRUE( bgr0_png.has_value() ) << bgr0_png.error().message;
    expect_equal_bytes( *rgb_png, *bgr0_png );
}
