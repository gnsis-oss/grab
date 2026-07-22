#include "codec/png.hpp"
#include "drivers/desktop/x11/workflow.hpp"
#include "grab/config.hpp"
#include "grab/result.hpp"
#include "grab/screen.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
// clang-format on

namespace
{

    constexpr const char*      xvfbDisplay                = ":90";
    constexpr int              xcbOk                      = 0;
    constexpr int              invalidScreenIndex         = 0;
    constexpr std::int16_t     windowX                    = 128;
    constexpr std::int16_t     windowY                    = 144;
    constexpr std::uint16_t    windowWidth                = 208U;
    constexpr std::uint16_t    windowHeight               = 136U;
    constexpr std::uint16_t    windowBorderWidth          = 0U;
    constexpr std::uint32_t    windowValueMask            = XCB_CW_BACK_PIXEL;
    constexpr std::uint8_t     propertyReplaceMode        = XCB_PROP_MODE_REPLACE;
    constexpr std::uint32_t    windowColor                = 0X00'2B'83'C6U;
    constexpr std::uint32_t    expectedPid                = 81'237U;
    constexpr std::uint32_t    missingPid                 = 99'731U;
    constexpr std::uint32_t    onePropertyItem            = 1U;
    constexpr std::uint8_t     format8Bits                = 8U;
    constexpr std::uint8_t     format32Bits               = 32U;
    constexpr std::size_t      singleWindowCount          = 1U;
    constexpr std::size_t      environmentTerminatorCount = 1U;
    constexpr auto             paintDelay            = std::chrono::milliseconds{ 50 };
    constexpr std::string_view windowInstance        = "grab-capture-window-instance";
    constexpr std::string_view windowClass           = "GrabCaptureWindowTargetClass";
    constexpr std::string_view windowClassMatch      = "capturewindowtarget";
    constexpr std::string_view windowTitle           = "grab window target title";
    constexpr std::string_view windowTitleMatch      = "window target";
    constexpr std::string_view missingClass          = "class-that-is-not-present";
    constexpr std::string_view netClientListAtom     = "_NET_CLIENT_LIST";
    constexpr std::string_view netWmNameAtom         = "_NET_WM_NAME";
    constexpr std::string_view netWmPidAtom          = "_NET_WM_PID";
    constexpr std::string_view utf8StringAtom        = "UTF8_STRING";
    constexpr std::string_view displayEnvironmentKey = "DISPLAY";
    constexpr std::string_view tempDirPrefix         = "grab-capture-window-id-";
    constexpr std::string_view outputFilename        = "captured-window.png";

    template<typename T>
    using XcbOwned = std::unique_ptr<T, decltype( &std::free )>;

    template<typename T>
    [[nodiscard]]
    XcbOwned<T>
    take_xcb_owned( T* pointer ) noexcept
    {
        return XcbOwned<T>{ pointer, &std::free };
    }

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

            char**                   original_environment_ = nullptr;
            std::vector<std::string> entries_;
            std::vector<char*>       environment_;
    };

    class TempDirectory
    {
        public:

            TempDirectory() :
                path_( std::filesystem::temp_directory_path() / unique_name() )
            {
                std::filesystem::create_directories( path_ );
            }

            ~TempDirectory()
            {
                std::error_code ignored;
                static_cast<void>( std::filesystem::remove_all( path_, ignored ) );
            }

            TempDirectory( const TempDirectory& ) = delete;
            TempDirectory&
            operator=( const TempDirectory& ) = delete;
            TempDirectory( TempDirectory&& )  = delete;
            TempDirectory&
            operator=( TempDirectory&& ) = delete;

            [[nodiscard]]
            std::filesystem::path
            file( std::string_view name ) const
            {
                return path_ / std::string{ name };
            }

        private:

            [[nodiscard]]
            static std::string
            unique_name()
            {
                const auto now = std::chrono::steady_clock::now().time_since_epoch();
                return std::string{ tempDirPrefix } + std::to_string( now.count() );
            }

            std::filesystem::path path_;
    };

    class TestConnection
    {
        public:

            explicit TestConnection( const char* display ) :
                connection_( xcb_connect( display,
                                          &screen_index_ ) )
            {
            }

            ~TestConnection()
            {
                if( connection_ != nullptr )
                {
                    xcb_disconnect( connection_ );
                }
            }

            TestConnection( const TestConnection& ) = delete;
            TestConnection&
            operator=( const TestConnection& ) = delete;
            TestConnection( TestConnection&& ) = delete;
            TestConnection&
            operator=( TestConnection&& ) = delete;

            [[nodiscard]]
            xcb_connection_t*
            get() const noexcept
            {
                return connection_;
            }

            [[nodiscard]]
            int
            screen_index() const noexcept
            {
                return screen_index_;
            }

        private:

            xcb_connection_t* connection_   = nullptr;
            int               screen_index_ = invalidScreenIndex;
    };

    [[nodiscard]]
    const xcb_screen_t*
    default_screen( xcb_connection_t* connection,
                    int               screen_index )
    {
        xcb_screen_iterator_t iterator =
            xcb_setup_roots_iterator( xcb_get_setup( connection ) );
        for( int current_screen = 0; current_screen < screen_index && iterator.rem > 0;
             ++current_screen )
        {
            xcb_screen_next( &iterator );
        }
        return iterator.data;
    }

    [[nodiscard]]
    testing::AssertionResult
    request_succeeded( xcb_connection_t* connection,
                       xcb_void_cookie_t cookie )
    {
        const auto error = take_xcb_owned( xcb_request_check( connection, cookie ) );
        if( error != nullptr )
        {
            return testing::AssertionFailure()
                << "X error " << static_cast<unsigned int>( error->error_code );
        }
        return testing::AssertionSuccess();
    }

    [[nodiscard]]
    testing::AssertionResult
    flush_succeeded( xcb_connection_t* connection )
    {
        if( xcb_flush( connection ) <=
            0 ||
            xcb_connection_has_error( connection ) != xcbOk )
        {
            return testing::AssertionFailure() << "xcb_flush failed";
        }
        return testing::AssertionSuccess();
    }

    [[nodiscard]]
    testing::AssertionResult
    intern_atom( xcb_connection_t* connection,
                 std::string_view  name,
                 xcb_atom_t&       atom )
    {
        xcb_generic_error_t* raw_error = nullptr;
        const auto           reply     = take_xcb_owned( xcb_intern_atom_reply(
            connection,
            xcb_intern_atom( connection,
                             0U,
                             static_cast<std::uint16_t>( name.size() ),
                             name.data() ),
            &raw_error
        ) );
        const auto           error     = take_xcb_owned( raw_error );
        if( error != nullptr || reply == nullptr )
        {
            return testing::AssertionFailure() << "xcb_intern_atom failed";
        }
        atom = reply->atom;
        return testing::AssertionSuccess();
    }

    [[nodiscard]]
    testing::AssertionResult
    set_property( xcb_connection_t* connection,
                  xcb_window_t      window,
                  xcb_atom_t        property,
                  xcb_atom_t        type,
                  std::uint8_t      format,
                  std::uint32_t     item_count,
                  const void*       data )
    {
        return request_succeeded( connection,
                                  xcb_change_property_checked( connection,
                                                               propertyReplaceMode,
                                                               window,
                                                               property,
                                                               type,
                                                               format,
                                                               item_count,
                                                               data ) );
    }

    [[nodiscard]]
    std::string
    wm_class_value( std::string_view instance,
                    std::string_view class_name )
    {
        std::string value{ instance };
        value.push_back( '\0' );
        value.append( class_name );
        value.push_back( '\0' );
        return value;
    }

    void
    set_window_properties( xcb_connection_t* connection,
                           xcb_window_t      window )
    {
        const std::string wm_class = wm_class_value( windowInstance, windowClass );
        EXPECT_TRUE( set_property( connection,
                                   window,
                                   XCB_ATOM_WM_CLASS,
                                   XCB_ATOM_STRING,
                                   format8Bits,
                                   static_cast<std::uint32_t>( wm_class.size() ),
                                   wm_class.data() ) );

        xcb_atom_t net_wm_name = XCB_ATOM_NONE;
        xcb_atom_t utf8_string = XCB_ATOM_NONE;
        ASSERT_TRUE( intern_atom( connection, netWmNameAtom, net_wm_name ) );
        ASSERT_TRUE( intern_atom( connection, utf8StringAtom, utf8_string ) );
        EXPECT_TRUE( set_property( connection,
                                   window,
                                   net_wm_name,
                                   utf8_string,
                                   format8Bits,
                                   static_cast<std::uint32_t>( windowTitle.size() ),
                                   windowTitle.data() ) );

        xcb_atom_t pid_atom = XCB_ATOM_NONE;
        ASSERT_TRUE( intern_atom( connection, netWmPidAtom, pid_atom ) );
        EXPECT_TRUE( set_property( connection,
                                   window,
                                   pid_atom,
                                   XCB_ATOM_CARDINAL,
                                   format32Bits,
                                   onePropertyItem,
                                   &expectedPid ) );
    }

    [[nodiscard]]
    xcb_window_t
    create_solid_window( xcb_connection_t*   connection,
                         const xcb_screen_t& screen )
    {
        const xcb_window_t window = xcb_generate_id( connection );
        const std::array<std::uint32_t, singleWindowCount> values{ windowColor };
        EXPECT_TRUE(
            request_succeeded( connection,
                               xcb_create_window_checked( connection,
                                                          screen.root_depth,
                                                          window,
                                                          screen.root,
                                                          windowX,
                                                          windowY,
                                                          windowWidth,
                                                          windowHeight,
                                                          windowBorderWidth,
                                                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                                          screen.root_visual,
                                                          windowValueMask,
                                                          values.data() ) )
        );
        set_window_properties( connection, window );
        EXPECT_TRUE( request_succeeded( connection,
                                        xcb_map_window_checked( connection, window ) ) );
        return window;
    }

    void
    set_client_list( xcb_connection_t* connection,
                     xcb_window_t      root,
                     xcb_window_t      window )
    {
        xcb_atom_t net_client_list = XCB_ATOM_NONE;
        ASSERT_TRUE( intern_atom( connection, netClientListAtom, net_client_list ) );
        const std::array<xcb_window_t, singleWindowCount> windows{ window };
        EXPECT_TRUE( set_property( connection,
                                   root,
                                   net_client_list,
                                   XCB_ATOM_WINDOW,
                                   format32Bits,
                                   static_cast<std::uint32_t>( windows.size() ),
                                   windows.data() ) );
    }

    [[nodiscard]]
    std::vector<std::byte>
    read_file( const std::filesystem::path& path )
    {
        std::ifstream     stream{ path, std::ios::binary };
        std::vector<char> bytes{
            std::istreambuf_iterator<char>{ stream },
            std::istreambuf_iterator<char>{}
        };

        std::vector<std::byte> result;
        result.reserve( bytes.size() );
        std::ranges::transform(
            bytes,
            std::back_inserter( result ),
            []( char value )
            {
                return static_cast<std::byte>( static_cast<unsigned char>( value ) );
            }
        );
        return result;
    }

    class CaptureWindowId : public testing::Test
    {
        protected:

            void
            SetUp() override
            {
                ASSERT_NE( connection_.get(), nullptr );
                ASSERT_EQ( xcb_connection_has_error( connection_.get() ), xcbOk );
                const xcb_screen_t* screen_info =
                    default_screen( connection_.get(), connection_.screen_index() );
                ASSERT_NE( screen_info, nullptr );

                window_ = create_solid_window( connection_.get(), *screen_info );
                set_client_list( connection_.get(), screen_info->root, window_ );
                ASSERT_TRUE( flush_succeeded( connection_.get() ) );
                std::this_thread::sleep_for( paintDelay );

                auto opened = grab::Screen::open( xvfbDisplay );
                ASSERT_TRUE( opened.has_value() ) << opened.error().message;
                screen_ = std::make_unique<grab::Screen>( std::move( *opened ) );
            }

            [[nodiscard]]
            grab::Screen&
            screen()
            {
                return *screen_;
            }

            [[nodiscard]]
            std::uint32_t
            window_id() const noexcept
            {
                return static_cast<std::uint32_t>( window_ );
            }

        private:

            ScopedEnvironment environment_{
                displayEnvironmentKey,
                std::string_view{ xvfbDisplay },
            };
            TestConnection                connection_{ xvfbDisplay };
            xcb_window_t                  window_ = XCB_WINDOW_NONE;
            std::unique_ptr<grab::Screen> screen_;
    };

}    // namespace

TEST_F( CaptureWindowId,
        ResolveByWmClass )
{
    const grab::config::TargetMatch match{
        .kind = grab::config::MatchKind::WmClass,
        .text = std::string{ windowClassMatch },
    };

    auto resolved = grab::screen::resolve_target( screen(), match );

    ASSERT_TRUE( resolved.has_value() ) << resolved.error().message;
    EXPECT_EQ( *resolved, window_id() );
}

TEST_F( CaptureWindowId,
        ResolveByTitleSubstring )
{
    const grab::config::TargetMatch match{
        .kind = grab::config::MatchKind::Title,
        .text = std::string{ windowTitleMatch },
    };

    auto resolved = grab::screen::resolve_target( screen(), match );

    ASSERT_TRUE( resolved.has_value() ) << resolved.error().message;
    EXPECT_EQ( *resolved, window_id() );
}

TEST_F( CaptureWindowId,
        ResolveByPid )
{
    const grab::config::TargetMatch match{
        .kind = grab::config::MatchKind::Pid,
        .text = {},
        .pid  = expectedPid,
    };

    auto resolved = grab::screen::resolve_target( screen(), match );

    ASSERT_TRUE( resolved.has_value() ) << resolved.error().message;
    EXPECT_EQ( *resolved, window_id() );
}

TEST_F( CaptureWindowId,
        ResolveByWindowId )
{
    const grab::config::TargetMatch match{
        .kind      = grab::config::MatchKind::WindowId,
        .text      = {},
        .window_id = window_id(),
    };

    auto resolved = grab::screen::resolve_target( screen(), match );

    ASSERT_TRUE( resolved.has_value() ) << resolved.error().message;
    EXPECT_EQ( *resolved, window_id() );
}

TEST_F( CaptureWindowId,
        ResolveZeroMatchesIsNotFound )
{
    const grab::config::TargetMatch class_match{
        .kind = grab::config::MatchKind::WmClass,
        .text = std::string{ missingClass },
    };
    auto class_result = grab::screen::resolve_target( screen(), class_match );

    ASSERT_FALSE( class_result.has_value() );
    EXPECT_EQ( class_result.error().code, grab::ErrorCode::WindowNotFound );

    const grab::config::TargetMatch pid_match{
        .kind = grab::config::MatchKind::Pid,
        .text = {},
        .pid  = missingPid,
    };
    auto pid_result = grab::screen::resolve_target( screen(), pid_match );

    ASSERT_FALSE( pid_result.has_value() );
    EXPECT_EQ( pid_result.error().code, grab::ErrorCode::WindowNotFound );
}

TEST_F( CaptureWindowId,
        CaptureWindowIdWritesPng )
{
    const TempDirectory temp;
    const auto          output_path = temp.file( outputFilename );

    auto                captured =
        grab::screen::capture_window_to( screen(), window_id(), output_path.string() );

    ASSERT_TRUE( captured.has_value() ) << captured.error().message;
    ASSERT_TRUE( std::filesystem::exists( output_path ) );
    auto decoded = grab::codec::decode_png( read_file( output_path ) );
    ASSERT_TRUE( decoded.has_value() ) << decoded.error().message;
    EXPECT_EQ( decoded->width, windowWidth );
    EXPECT_EQ( decoded->height, windowHeight );
}
