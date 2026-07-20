#include "grab/config.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>
// clang-format on

namespace
{

    constexpr std::size_t      depthCap                   = 64U;
    constexpr std::size_t      aboveDepthCap              = depthCap + 1U;
    constexpr double           defaultTimeout             = 15.0;
    constexpr std::uint16_t    defaultWidth               = 1'920U;
    constexpr std::uint16_t    defaultHeight              = 1'080U;
    constexpr std::uint8_t     defaultDepth               = 24U;
    constexpr std::uint32_t    defaultPopupTime           = 2'000U;
    constexpr double           defaultThreshold           = 5.0;
    constexpr std::uint32_t    expectedWindowId           = 0X01'40'00'03U;
    constexpr std::uint32_t    fallbackFrames             = 1U;
    constexpr std::uint32_t    overrideFrames             = 3U;
    constexpr std::size_t      expectedTargetCount        = 2U;
    constexpr std::size_t      resolvedFileCount          = 1U;
    constexpr std::size_t      environmentTerminatorCount = 1U;
    constexpr std::string_view schemaOnly                 = R"({"schema_version":1})";

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

            [[nodiscard]]
            const std::filesystem::path&
            directory() const noexcept
            {
                return root_;
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

    class ScopedEnvironment
    {
        public:

            ScopedEnvironment( std::string_view                name,
                               std::optional<std::string_view> value ) :
                original_environment_( ::environ )
            {
                for( char* const* entry = original_environment_;
                     entry != nullptr && *entry != nullptr;
                     entry = std::next( entry ) )
                {
                    const std::string_view current{ *entry };
                    const auto             separator = current.find( '=' );
                    if( current.substr( 0U, separator ) != name )
                    {
                        entries_.emplace_back( current );
                    }
                }
                if( value.has_value() )
                {
                    entries_.emplace_back(
                        std::string{ name } + "=" + std::string{ *value }
                    );
                }

                environment_.reserve( entries_.size() + environmentTerminatorCount );
                for( std::string& entry : entries_ )
                {
                    environment_.push_back( entry.data() );
                }
                environment_.push_back( nullptr );
                ::environ = environment_.data();
            }

            ~ScopedEnvironment()
            {
                ::environ = original_environment_;
            }

            ScopedEnvironment( const ScopedEnvironment& ) = delete;
            ScopedEnvironment&
            operator=( const ScopedEnvironment& )    = delete;
            ScopedEnvironment( ScopedEnvironment&& ) = delete;
            ScopedEnvironment&
            operator=( ScopedEnvironment&& ) = delete;

        private:

            char**                   original_environment_{};
            std::vector<std::string> entries_;
            std::vector<char*>       environment_;
    };

    void
    expect_invalid_at( const grab::Result<grab::config::Config>& result,
                       std::string_view                          pointer )
    {
        ASSERT_FALSE( result.has_value() );
        EXPECT_EQ( result.error().code, grab::ErrorCode::InvalidArgument );
        EXPECT_NE( result.error().message.find( pointer ), std::string::npos );
    }

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

TEST( ConfigLoad,
      UnknownKeyIsError )
{
    const TempConfig file{ R"({
        "schema_version": 1,
        "watch": {"interval_ms": 20, "output": "shots", "intervall_ms": 20}
    })" };

    const auto       result = grab::config::load( file.path() );

    expect_invalid_at( result, "/watch/intervall_ms" );
}

TEST( ConfigLoad,
      UnsupportedSchemaVersion )
{
    const TempConfig file{ R"({"schema_version":2,"unexpected":true})" };

    const auto       result = grab::config::load( file.path() );

    expect_invalid_at( result, "/schema_version" );
    EXPECT_NE( result.error().message.find( "unsupported version" ), std::string::npos );
    EXPECT_EQ( result.error().message.find( "unknown key" ), std::string::npos );
}

TEST( ConfigLoad,
      IntegerFieldRejectsFloat )
{
    const TempConfig file{ R"({
        "schema_version": 1,
        "watch": {"interval_ms": 20.0, "output": "shots"}
    })" };

    const auto       result = grab::config::load( file.path() );

    expect_invalid_at( result, "/watch/interval_ms" );
}

TEST( ConfigLoad,
      TargetMatchExactlyOneKey )
{
    {
        const TempConfig file{ R"({
            "schema_version": 1,
            "watch": {
                "interval_ms": 20,
                "output": "shots",
                "target": {"title": "editor", "wm_class": "Editor"}
            }
        })" };

        const auto       result = grab::config::load( file.path() );
        expect_invalid_at( result, "/watch/target" );
    }

    {
        const TempConfig file{ R"({
            "schema_version": 1,
            "watch": {
                "interval_ms": 20,
                "output": "shots",
                "target": {}
            }
        })" };

        const auto       result = grab::config::load( file.path() );
        expect_invalid_at( result, "/watch/target" );
    }
}

TEST( ConfigLoad,
      WindowIdParsesHex )
{
    {
        const TempConfig file{ R"({
            "schema_version": 1,
            "watch": {
                "interval_ms": 20,
                "output": "shots",
                "target": {"window_id": "0x1400003"}
            }
        })" };

        const auto       result = grab::config::load( file.path() );
        ASSERT_TRUE( result.has_value() ) << result.error().message;
        ASSERT_TRUE( result->watch.has_value() );
        ASSERT_TRUE( result->watch->target.has_value() );
        EXPECT_EQ( result->watch->target->kind, grab::config::MatchKind::WindowId );
        EXPECT_EQ( result->watch->target->window_id, expectedWindowId );
    }

    for( const std::string_view value : { "1400003", "0xnot-hex" } )
    {
        const std::string document =
            std::string{
                R"({"schema_version":1,"watch":{"interval_ms":20,"output":"shots","target":{"window_id":")"
            } +
            std::string{ value } +
            R"("}}})";
        const TempConfig file{ document };

        const auto       result = grab::config::load( file.path() );
        expect_invalid_at( result, "/watch/target/window_id" );
    }
}

TEST( ConfigLoad,
      ScriptLoopRequiresDelay )
{
    const TempConfig file{ R"({
        "schema_version": 1,
        "script": {
            "loop": true,
            "steps": [
                {"action": "move", "x": 1, "y": 2},
                {"action": "delay", "ms": 99}
            ]
        }
    })" };

    const auto       result = grab::config::load( file.path() );

    expect_invalid_at( result, "/script/steps" );
}

TEST( ConfigLoad,
      ArgvMustBeNonEmpty )
{
    const TempConfig file{ R"({
        "schema_version": 1,
        "targets": [{"name": "empty", "argv": []}]
    })" };

    const auto       result = grab::config::load( file.path() );

    expect_invalid_at( result, "/targets/0/argv" );
}

TEST( ConfigLoad,
      TargetEnvDisplayForbiddenWithXvfb )
{
    const TempConfig file{ R"({
        "schema_version": 1,
        "display": {"backend": "xvfb"},
        "targets": [{
            "name": "editor",
            "argv": ["editor"],
            "env": {"DISPLAY": ":42"}
        }]
    })" };

    const auto       result = grab::config::load( file.path() );

    expect_invalid_at( result, "/targets/0/env/DISPLAY" );
}

TEST( ConfigLoad,
      PatternRequiredForWmClassMatch )
{
    const TempConfig file{ R"({
        "schema_version": 1,
        "targets": [{"name": "editor", "argv": ["editor"], "match": "wm_class"}]
    })" };

    const auto       result = grab::config::load( file.path() );

    expect_invalid_at( result, "/targets/0/pattern" );
}

TEST( ConfigLoad,
      PatternForbiddenForPidMatch )
{
    const TempConfig file{ R"({
        "schema_version": 1,
        "targets": [{"name": "editor", "argv": ["editor"], "pattern": "Editor"}]
    })" };

    const auto       result = grab::config::load( file.path() );

    expect_invalid_at( result, "/targets/0/pattern" );
}

TEST( ConfigLoad,
      TildeExpandsToHome )
{
    const TempConfig        file{ R"({
        "schema_version": 1,
        "watch": {"interval_ms": 20, "output": "~/captures"},
        "compare": {"ref": "~//reference"}
    })" };
    const std::string       home = file.directory().string();
    const ScopedEnvironment environment{ "HOME", home };

    const auto              result = grab::config::load( file.path() );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    ASSERT_TRUE( result->watch.has_value() );
    ASSERT_TRUE( result->compare.ref.has_value() );
    EXPECT_EQ( result->watch->output, file.directory() / "captures" );
    EXPECT_EQ( *result->compare.ref, file.directory() / "reference" );
}

TEST( ConfigLoad,
      RelativePathsResolveAgainstConfigDir )
{
    const TempConfig file{ R"({
        "schema_version": 1,
        "watch": {"interval_ms": 20, "output": "shots"},
        "compare": {"ref": "reference"}
    })" };

    std::error_code  relative_error;
    const auto       relative_path =
        std::filesystem::relative( file.path(),
                                   std::filesystem::current_path(),
                                   relative_error );
    ASSERT_FALSE( relative_error );

    const auto result = grab::config::load( relative_path );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    ASSERT_TRUE( result->watch.has_value() );
    ASSERT_TRUE( result->compare.ref.has_value() );
    EXPECT_EQ( result->watch->output, file.directory() / "shots" );
    EXPECT_EQ( *result->compare.ref, file.directory() / "reference" );
}

TEST( ConfigLoad,
      DefaultsApplyToTargets )
{
    const TempConfig file{ R"({
        "schema_version": 1,
        "defaults": {"timeout_s": 30, "kill_after": false},
        "targets": [
            {"name": "fallback", "argv": ["fallback"]},
            {"name": "override", "argv": ["override"],
             "frames": 3, "timeout_s": 45, "kill_after": true}
        ]
    })" };
    constexpr double fallbackTimeout = 30.0;
    constexpr double overrideTimeout = 45.0;

    const auto       result          = grab::config::load( file.path() );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    ASSERT_EQ( result->targets.size(), expectedTargetCount );
    EXPECT_DOUBLE_EQ( result->targets.front().timeout_s, fallbackTimeout );
    EXPECT_FALSE( result->targets.front().kill_after );
    EXPECT_EQ( result->targets.front().frames, fallbackFrames );
    EXPECT_DOUBLE_EQ( result->targets.back().timeout_s, overrideTimeout );
    EXPECT_TRUE( result->targets.back().kill_after );
    EXPECT_EQ( result->targets.back().frames, overrideFrames );
}

TEST( ConfigLoad,
      OutputRootPrefixesRelativeOutput )
{
    const TempConfig file{ R"({
        "schema_version": 1,
        "defaults": {"output_root": "artifacts"},
        "watch": {"interval_ms": 20, "output": "watch"},
        "batch": {"output_root": "batch"}
    })" };

    const auto       result = grab::config::load( file.path() );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    ASSERT_TRUE( result->watch.has_value() );
    const auto root = file.directory() / "artifacts";
    EXPECT_EQ( result->defaults.output_root, root );
    EXPECT_EQ( result->watch->output, root / "watch" );
    EXPECT_EQ( result->batch.output_root, root / "batch" );
}

TEST( ConfigLoad,
      DeferredBackendRejected )
{
    const TempConfig file{ R"({
        "schema_version": 1,
        "display": {"backend": "weston"}
    })" };

    const auto       result = grab::config::load( file.path() );

    expect_invalid_at( result, "/display/backend" );
    EXPECT_NE( result.error().message.find( "deferred" ), std::string::npos );
}

TEST( ConfigLoad,
      NotifyTruthTable )
{
    {
        const TempConfig file{ R"({
            "schema_version": 1,
            "notifications": {"enabled": false, "strategy": "os"}
        })" };
        const auto       result = grab::config::load( file.path() );
        ASSERT_TRUE( result.has_value() ) << result.error().message;
        EXPECT_FALSE( result->notifications.enabled );
        EXPECT_EQ( result->notifications.strategy, grab::config::NotifyStrategy::Os );
    }

    {
        const TempConfig file{ R"({
            "schema_version": 1,
            "notifications": {"enabled": true, "strategy": "none"}
        })" };
        const auto       result = grab::config::load( file.path() );
        ASSERT_TRUE( result.has_value() ) << result.error().message;
        EXPECT_TRUE( result->notifications.enabled );
        EXPECT_EQ( result->notifications.strategy, grab::config::NotifyStrategy::None );
    }
}

TEST( ConfigLoad,
      ResolveUsesGrabConfigEnvWhenNoPaths )
{
    const TempConfig                       file{ schemaOnly };
    const std::string                      config_path = file.path().string();
    const ScopedEnvironment                environment{ "GRAB_CONFIG", config_path };
    const std::array<std::string_view, 0U> no_paths{};

    const auto                             result = grab::config::resolve( no_paths );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    ASSERT_EQ( result->size(), resolvedFileCount );
    EXPECT_EQ( result->front().source, file.path() );
}

TEST( ConfigLoad,
      ResolveErrorsWithNoPathsAndNoEnv )
{
    const ScopedEnvironment                environment{ "GRAB_CONFIG", std::nullopt };
    const std::array<std::string_view, 0U> no_paths{};

    const auto                             result = grab::config::resolve( no_paths );

    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, grab::ErrorCode::InvalidArgument );
    EXPECT_NE( result.error().message.find( '/' ), std::string::npos );
}

TEST( ConfigLoad,
      WholeFileValidatesRegardlessOfVerb )
{
    const TempConfig file{ R"({
        "schema_version": 1,
        "watch": {"interval_ms": 20, "output": "shots"},
        "targets": [{"name": "broken", "argv": []}]
    })" };

    const auto       result = grab::config::load( file.path() );

    expect_invalid_at( result, "/targets/0/argv" );
}
