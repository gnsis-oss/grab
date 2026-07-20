#include "codec/png.hpp"
#include "grab/image.hpp"
#include "grab/result.hpp"
#include "image/compare_dirs.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    constexpr std::uint32_t knownWidth            = 2U;
    constexpr std::uint32_t knownHeight           = 1U;
    constexpr std::uint32_t mismatchedWidth       = 3U;
    constexpr std::uint32_t rgbaBytes             = 4U;
    constexpr std::uint32_t rgbaChannelSquareRoot = 2U;
    constexpr std::uint32_t grayBytes             = 1U;
    constexpr std::uint8_t  baseRed               = 40U;
    constexpr std::uint8_t  baseGreen             = 80U;
    constexpr std::uint8_t  baseBlue              = 120U;
    constexpr std::uint8_t  baseAlpha             = 160U;
    constexpr std::uint8_t  knownChannelDelta     = 10U;
    constexpr std::uint8_t  changedRed =
        static_cast<std::uint8_t>( baseRed + knownChannelDelta );
    constexpr std::uint8_t baseGray        = 30U;
    constexpr std::uint8_t pairedDarkGray  = 20U;
    constexpr std::uint8_t pairedLightGray = 200U;
    constexpr std::uint8_t underDelta      = 4U;
    constexpr std::uint8_t overDelta       = 6U;
    constexpr std::uint8_t underGray =
        static_cast<std::uint8_t>( baseGray + underDelta );
    constexpr std::uint8_t overGray  = static_cast<std::uint8_t>( baseGray + overDelta );
    constexpr double       zeroScore = 0.0;
    constexpr double expectedKnownRmse = static_cast<double>( knownChannelDelta ) /
                                         static_cast<double>( rgbaChannelSquareRoot );
    constexpr double scoreTolerance    = 1.0E-12;
    constexpr double exactThreshold    = 0.0;
    constexpr double rmseThreshold     = 5.0;
    constexpr std::size_t      pairedFileCount      = 2U;
    constexpr std::size_t      singleFileCount      = 1U;
    constexpr auto             invalidArgument      = grab::ErrorCode::InvalidArgument;
    constexpr std::string_view tempDirectoryPrefix  = "grab-compare-dirs-";
    constexpr std::string_view testNameSeparator    = "-";
    constexpr std::string_view refDirectoryName     = "ref";
    constexpr std::string_view currentDirectoryName = "current";
    constexpr std::string_view alphaFileName        = "alpha.png";
    constexpr std::string_view betaFileName         = "beta.png";
    constexpr std::string_view missingFileName      = "missing.png";
    constexpr std::string_view extraFileName        = "extra.png";
    constexpr std::string_view exactFileName        = "exact.png";
    constexpr std::string_view underFileName        = "under.png";
    constexpr std::string_view overFileName         = "over.png";

    [[nodiscard]]
    std::byte
    byte_from( std::uint8_t value ) noexcept
    {
        return static_cast<std::byte>( value );
    }

    [[nodiscard]]
    grab::Image
    make_rgba_image( std::uint32_t width,
                     std::uint32_t height,
                     std::uint8_t  red,
                     std::uint8_t  green,
                     std::uint8_t  blue,
                     std::uint8_t  alpha )
    {
        const auto stride = width * rgbaBytes;
        const auto pixel_count =
            static_cast<std::size_t>( width ) * static_cast<std::size_t>( height );
        std::vector<std::byte> pixels;
        pixels.reserve( pixel_count * static_cast<std::size_t>( rgbaBytes ) );
        for( std::size_t pixel_index = 0U; pixel_index < pixel_count; ++pixel_index )
        {
            pixels.push_back( byte_from( red ) );
            pixels.push_back( byte_from( green ) );
            pixels.push_back( byte_from( blue ) );
            pixels.push_back( byte_from( alpha ) );
        }

        return grab::Image{
            .width  = width,
            .height = height,
            .stride = stride,
            .format = grab::PixelFormat::Rgba,
            .pixels = std::move( pixels ),
        };
    }

    [[nodiscard]]
    grab::Image
    make_gray_image( std::uint8_t value )
    {
        const auto stride = knownWidth * grayBytes;
        return grab::Image{
            .width  = knownWidth,
            .height = knownHeight,
            .stride = stride,
            .format = grab::PixelFormat::Gray,
            .pixels =
                std::vector<std::byte>( static_cast<std::size_t>( stride ) *
                                            static_cast<std::size_t>( knownHeight ),
                                        byte_from( value ) ),
        };
    }

    [[nodiscard]]
    grab::Result<void>
    write_png_file( const std::filesystem::path& path,
                    const grab::Image&           image )
    {
        auto encoded = grab::codec::encode_png( image );
        if( !encoded.has_value() )
        {
            return std::unexpected( std::move( encoded.error() ) );
        }

        std::ofstream stream{ path, std::ios::binary | std::ios::trunc };
        if( !stream )
        {
            return grab::fail( grab::ErrorCode::ProviderFailed,
                               "failed to open test PNG: " + path.string() );
        }
        for( const std::byte value : *encoded )
        {
            stream.put( static_cast<char>( std::to_integer<unsigned char>( value ) ) );
        }
        if( !stream )
        {
            return grab::fail( grab::ErrorCode::ProviderFailed,
                               "failed to write test PNG: " + path.string() );
        }
        return {};
    }

    class TempDirectories
    {
        public:

            TempDirectories() :
                root_( std::filesystem::temp_directory_path() / unique_name() ),
                ref_( root_ / std::string{ refDirectoryName } ),
                current_( root_ / std::string{ currentDirectoryName } )
            {
                std::error_code error;
                static_cast<void>( std::filesystem::remove_all( root_, error ) );
                EXPECT_FALSE( error );

                error.clear();
                const bool ref_created =
                    std::filesystem::create_directories( ref_, error );
                EXPECT_TRUE( ref_created );
                EXPECT_FALSE( error );

                error.clear();
                const bool current_created =
                    std::filesystem::create_directories( current_, error );
                EXPECT_TRUE( current_created );
                EXPECT_FALSE( error );
            }

            ~TempDirectories() noexcept
            {
                std::error_code error;
                static_cast<void>( std::filesystem::remove_all( root_, error ) );
            }

            TempDirectories( const TempDirectories& ) = delete;
            TempDirectories&
            operator=( const TempDirectories& )  = delete;
            TempDirectories( TempDirectories&& ) = delete;
            TempDirectories&
            operator=( TempDirectories&& ) = delete;

            [[nodiscard]]
            const std::filesystem::path&
            ref() const noexcept
            {
                return ref_;
            }

            [[nodiscard]]
            const std::filesystem::path&
            current() const noexcept
            {
                return current_;
            }

            [[nodiscard]]
            std::filesystem::path
            ref_file( std::string_view name ) const
            {
                return ref_ / std::string{ name };
            }

            [[nodiscard]]
            std::filesystem::path
            current_file( std::string_view name ) const
            {
                return current_ / std::string{ name };
            }

        private:

            [[nodiscard]]
            static std::string
            unique_name()
            {
                const auto* info = testing::UnitTest::GetInstance()->current_test_info();
                return std::string{ tempDirectoryPrefix } +
                       info->test_suite_name() +
                       std::string{ testNameSeparator } +
                       info->name();
            }

            std::filesystem::path root_;
            std::filesystem::path ref_;
            std::filesystem::path current_;
    };

    [[nodiscard]]
    const grab::image::FileCompareResult*
    find_result( const std::vector<grab::image::FileCompareResult>& results,
                 std::string_view                                   name )
    {
        const auto found =
            std::ranges::find_if( results,
                                  [name]( const grab::image::FileCompareResult& result )
                                  {
                                      return result.name == name;
                                  } );
        if( found == results.end() )
        {
            return nullptr;
        }
        return &*found;
    }

}    // namespace

TEST( CompareDirs,
      RmseZeroForIdentical )
{
    const auto first  = make_rgba_image( knownWidth,
                                         knownHeight,
                                         baseRed,
                                         baseGreen,
                                         baseBlue,
                                         baseAlpha );
    const auto second = make_rgba_image( knownWidth,
                                         knownHeight,
                                         baseRed,
                                         baseGreen,
                                         baseBlue,
                                         baseAlpha );

    const auto result = grab::image::rmse( first, second );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    EXPECT_DOUBLE_EQ( *result, zeroScore );
}

TEST( CompareDirs,
      RmseKnownValue )
{
    const auto first  = make_rgba_image( knownWidth,
                                         knownHeight,
                                         baseRed,
                                         baseGreen,
                                         baseBlue,
                                         baseAlpha );
    const auto second = make_rgba_image( knownWidth,
                                         knownHeight,
                                         changedRed,
                                         baseGreen,
                                         baseBlue,
                                         baseAlpha );

    const auto result = grab::image::rmse( first, second );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    EXPECT_NEAR( *result, expectedKnownRmse, scoreTolerance );
}

TEST( CompareDirs,
      RmseSizeMismatchErrors )
{
    const auto first  = make_rgba_image( knownWidth,
                                         knownHeight,
                                         baseRed,
                                         baseGreen,
                                         baseBlue,
                                         baseAlpha );
    const auto second = make_rgba_image( mismatchedWidth,
                                         knownHeight,
                                         baseRed,
                                         baseGreen,
                                         baseBlue,
                                         baseAlpha );

    const auto result = grab::image::rmse( first, second );

    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, invalidArgument );
}

TEST( CompareDirs,
      CompareDirsPairsByName )
{
    const TempDirectories directories;
    const auto            dark  = make_gray_image( pairedDarkGray );
    const auto            light = make_gray_image( pairedLightGray );

    const auto ref_alpha = write_png_file( directories.ref_file( alphaFileName ), dark );
    ASSERT_TRUE( ref_alpha.has_value() ) << ref_alpha.error().message;
    const auto ref_beta = write_png_file( directories.ref_file( betaFileName ), light );
    ASSERT_TRUE( ref_beta.has_value() ) << ref_beta.error().message;
    const auto current_beta =
        write_png_file( directories.current_file( betaFileName ), light );
    ASSERT_TRUE( current_beta.has_value() ) << current_beta.error().message;
    const auto current_alpha =
        write_png_file( directories.current_file( alphaFileName ), dark );
    ASSERT_TRUE( current_alpha.has_value() ) << current_alpha.error().message;

    const auto result = grab::image::compare_dirs( directories.ref(),
                                                   directories.current(),
                                                   grab::image::DirCompareMode::Exact,
                                                   exactThreshold );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    ASSERT_EQ( result->size(), pairedFileCount );
    const auto* alpha = find_result( *result, alphaFileName );
    ASSERT_NE( alpha, nullptr );
    EXPECT_TRUE( alpha->in_ref );
    EXPECT_TRUE( alpha->in_current );
    EXPECT_TRUE( alpha->passed );
    const auto* beta = find_result( *result, betaFileName );
    ASSERT_NE( beta, nullptr );
    EXPECT_TRUE( beta->in_ref );
    EXPECT_TRUE( beta->in_current );
    EXPECT_TRUE( beta->passed );
}

TEST( CompareDirs,
      MissingInCurrentFails )
{
    const TempDirectories directories;
    const auto            image = make_gray_image( baseGray );
    const auto            written =
        write_png_file( directories.ref_file( missingFileName ), image );
    ASSERT_TRUE( written.has_value() ) << written.error().message;

    const auto result = grab::image::compare_dirs( directories.ref(),
                                                   directories.current(),
                                                   grab::image::DirCompareMode::Exact,
                                                   exactThreshold );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    ASSERT_EQ( result->size(), singleFileCount );
    EXPECT_EQ( result->front().name, missingFileName );
    EXPECT_TRUE( result->front().in_ref );
    EXPECT_FALSE( result->front().in_current );
    EXPECT_FALSE( result->front().passed );
}

TEST( CompareDirs,
      ExtraInCurrentFails )
{
    const TempDirectories directories;
    const auto            image = make_gray_image( baseGray );
    const auto            written =
        write_png_file( directories.current_file( extraFileName ), image );
    ASSERT_TRUE( written.has_value() ) << written.error().message;

    const auto result = grab::image::compare_dirs( directories.ref(),
                                                   directories.current(),
                                                   grab::image::DirCompareMode::Exact,
                                                   exactThreshold );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    ASSERT_EQ( result->size(), singleFileCount );
    EXPECT_EQ( result->front().name, extraFileName );
    EXPECT_FALSE( result->front().in_ref );
    EXPECT_TRUE( result->front().in_current );
    EXPECT_FALSE( result->front().passed );
}

TEST( CompareDirs,
      ExactModeZeroDiff )
{
    const TempDirectories directories;
    const auto            image = make_gray_image( baseGray );
    const auto            ref_written =
        write_png_file( directories.ref_file( exactFileName ), image );
    ASSERT_TRUE( ref_written.has_value() ) << ref_written.error().message;
    const auto current_written =
        write_png_file( directories.current_file( exactFileName ), image );
    ASSERT_TRUE( current_written.has_value() ) << current_written.error().message;

    const auto result = grab::image::compare_dirs( directories.ref(),
                                                   directories.current(),
                                                   grab::image::DirCompareMode::Exact,
                                                   exactThreshold );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    ASSERT_EQ( result->size(), singleFileCount );
    EXPECT_DOUBLE_EQ( result->front().score, zeroScore );
    EXPECT_TRUE( result->front().passed );
}

TEST( CompareDirs,
      RmseModeThreshold )
{
    const TempDirectories directories;
    const auto            base  = make_gray_image( baseGray );
    const auto            under = make_gray_image( underGray );
    const auto            over  = make_gray_image( overGray );

    const auto ref_under = write_png_file( directories.ref_file( underFileName ), base );
    ASSERT_TRUE( ref_under.has_value() ) << ref_under.error().message;
    const auto ref_over = write_png_file( directories.ref_file( overFileName ), base );
    ASSERT_TRUE( ref_over.has_value() ) << ref_over.error().message;
    const auto current_under =
        write_png_file( directories.current_file( underFileName ), under );
    ASSERT_TRUE( current_under.has_value() ) << current_under.error().message;
    const auto current_over =
        write_png_file( directories.current_file( overFileName ), over );
    ASSERT_TRUE( current_over.has_value() ) << current_over.error().message;

    const auto result = grab::image::compare_dirs( directories.ref(),
                                                   directories.current(),
                                                   grab::image::DirCompareMode::Rmse,
                                                   rmseThreshold );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    ASSERT_EQ( result->size(), pairedFileCount );
    const auto* under_result = find_result( *result, underFileName );
    ASSERT_NE( under_result, nullptr );
    EXPECT_NEAR( under_result->score,
                 static_cast<double>( underDelta ),
                 scoreTolerance );
    EXPECT_TRUE( under_result->passed );
    const auto* over_result = find_result( *result, overFileName );
    ASSERT_NE( over_result, nullptr );
    EXPECT_NEAR( over_result->score, static_cast<double>( overDelta ), scoreTolerance );
    EXPECT_FALSE( over_result->passed );
}
