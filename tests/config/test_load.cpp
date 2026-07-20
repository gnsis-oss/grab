#include "grab/config.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
// clang-format on

namespace
{

    constexpr std::size_t      depthCap         = 64U;
    constexpr std::size_t      aboveDepthCap    = depthCap + 1U;
    constexpr double           defaultTimeout   = 15.0;
    constexpr std::uint16_t    defaultWidth     = 1'920U;
    constexpr std::uint16_t    defaultHeight    = 1'080U;
    constexpr std::uint8_t     defaultDepth     = 24U;
    constexpr std::uint32_t    defaultPopupTime = 2'000U;
    constexpr double           defaultThreshold = 5.0;
    constexpr std::string_view schemaOnly       = R"({"schema_version":1})";

    class TempConfig
    {
        public:

            explicit TempConfig( std::string_view contents ) :
                root_( std::filesystem::temp_directory_path() / unique_name() ),
                path_( root_ / "config.json" )
            {
                std::error_code error;
                static_cast<void>( std::filesystem::remove_all( root_, error ) );
                error.clear();
                const bool created = std::filesystem::create_directories( root_, error );
                EXPECT_TRUE( created );
                EXPECT_FALSE( error );

                std::ofstream stream{ path_ };
                stream << contents;
                EXPECT_TRUE( stream.good() );
            }

            ~TempConfig() noexcept
            {
                std::error_code error;
                static_cast<void>( std::filesystem::remove_all( root_, error ) );
            }

            TempConfig( const TempConfig& ) = delete;
            TempConfig&
            operator=( const TempConfig& ) = delete;
            TempConfig( TempConfig&& )     = delete;
            TempConfig&
            operator=( TempConfig&& ) = delete;

            [[nodiscard]]
            const std::filesystem::path&
            path() const noexcept
            {
                return path_;
            }

        private:

            [[nodiscard]]
            static std::string
            unique_name()
            {
                const auto* info = testing::UnitTest::GetInstance()->current_test_info();
                return std::string{ "grab-config-" } +
                       info->test_suite_name() +
                       "-" +
                       info->name();
            }

            std::filesystem::path root_;
            std::filesystem::path path_;
    };

    [[nodiscard]]
    std::string
    nested_arrays( std::size_t depth )
    {
        std::string document( depth, '[' );
        document += "null";
        document.append( depth, ']' );
        return document;
    }

}    // namespace

TEST( ConfigLoad,
      RejectsComment )
{
    const TempConfig file{ R"({"schema_version":1 // comment
})" };

    const auto       result = grab::config::load( file.path() );

    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, grab::ErrorCode::InvalidArgument );
}

TEST( ConfigLoad,
      RejectsDuplicateKey )
{
    const TempConfig file{ R"({"a":1,"a":2})" };

    const auto       result = grab::config::load( file.path() );

    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, grab::ErrorCode::InvalidArgument );
    EXPECT_NE( result.error().message.find( "duplicate key" ), std::string::npos );
}

TEST( ConfigLoad,
      RejectsDepthAboveCap )
{
    const TempConfig file{ nested_arrays( aboveDepthCap ) };

    const auto       result = grab::config::load( file.path() );

    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, grab::ErrorCode::InvalidArgument );
    EXPECT_NE( result.error().message.find( "nesting depth" ), std::string::npos );
}

TEST( ConfigLoad,
      AcceptsDepthAtCap )
{
    const TempConfig file{ nested_arrays( depthCap ) };

    const auto       result = grab::config::load( file.path() );

    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, grab::ErrorCode::InvalidArgument );
    EXPECT_EQ( result.error().message.find( "nesting depth" ), std::string::npos );
}

TEST( ConfigLoad,
      ParseErrorCarriesByteOffset )
{
    const TempConfig file{ R"({"schema_version":})" };

    const auto       result = grab::config::load( file.path() );

    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, grab::ErrorCode::InvalidArgument );
    EXPECT_NE( result.error().message.find( "byte" ), std::string::npos );
}

TEST( ConfigLoad,
      MissingFileIsNotFound )
{
    const auto missing =
        std::filesystem::temp_directory_path() / "grab-config-definitely-missing.json";
    std::error_code error;
    static_cast<void>( std::filesystem::remove( missing, error ) );

    const auto result = grab::config::load( missing );

    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, grab::ErrorCode::InvalidArgument );
}

TEST( ConfigLoad,
      MinimalConfigLoads )
{
    const TempConfig file{ schemaOnly };

    const auto       result = grab::config::load( file.path() );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    EXPECT_EQ( result->source, file.path() );
    EXPECT_EQ( result->defaults.format, "png" );
    EXPECT_DOUBLE_EQ( result->defaults.timeout_s, defaultTimeout );
    EXPECT_TRUE( result->defaults.kill_after );
    EXPECT_EQ( result->defaults.output_root, file.path().parent_path() );
    EXPECT_EQ( result->display.backend, grab::config::DisplayBackend::Native );
    EXPECT_EQ( result->display.width, defaultWidth );
    EXPECT_EQ( result->display.height, defaultHeight );
    EXPECT_EQ( result->display.depth, defaultDepth );
    EXPECT_FALSE( result->watch.has_value() );
    EXPECT_FALSE( result->script.has_value() );
    EXPECT_TRUE( result->targets.empty() );
    EXPECT_EQ( result->batch.output_root, file.path().parent_path() / "sessions" );
    EXPECT_FALSE( result->notifications.enabled );
    EXPECT_EQ( result->notifications.strategy, grab::config::NotifyStrategy::Os );
    EXPECT_EQ( result->notifications.popup_timeout_ms, defaultPopupTime );
    EXPECT_EQ( result->compare.mode, grab::config::CompareMode::Rmse );
    EXPECT_DOUBLE_EQ( result->compare.threshold, defaultThreshold );
    EXPECT_FALSE( result->compare.ref.has_value() );
}
