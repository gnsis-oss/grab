#include "codec/png.hpp"
#include "grab/image.hpp"
#include "grab/result.hpp"
#include "grab/screen.hpp"
#include "screen/workflow.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <future>
#include <ios>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
// clang-format on

namespace
{

    constexpr const char*      kXvfbDisplay         = ":87";
    constexpr int              kXcbOk               = 0;
    constexpr int              kInvalidScreenIndex  = 0;
    constexpr std::int16_t     kFirstWindowX        = 64;
    constexpr std::int16_t     kFirstWindowY        = 72;
    constexpr std::int16_t     kSecondWindowX       = 312;
    constexpr std::int16_t     kSecondWindowY       = 72;
    constexpr std::uint16_t    kWindowWidth         = 160U;
    constexpr std::uint16_t    kWindowHeight        = 96U;
    constexpr std::uint16_t    kWindowBorderWidth   = 0U;
    constexpr std::uint32_t    kWindowValueMask     = XCB_CW_BACK_PIXEL;
    constexpr std::uint32_t    kPropertyReplaceMode = XCB_PROP_MODE_REPLACE;
    constexpr std::uint32_t    kFirstColor          = 0X00'18'84'D8U;
    constexpr std::uint32_t    kSecondColor         = 0X00'D8'84'18U;
    constexpr std::uint8_t     kFormat8Bits         = 8U;
    constexpr std::uint8_t     kFormat32Bits        = 32U;
    constexpr std::uint32_t    kExpectedCaptured    = 2U;
    constexpr std::uint32_t    kMinimumWatchCapture = 1U;
    constexpr std::uint64_t    kNoDiffPixels        = 0U;
    constexpr double           kFullMatchRatio      = 1.0;
    constexpr std::uint32_t    kImageWidth          = 6U;
    constexpr std::uint32_t    kImageHeight         = 4U;
    constexpr std::uint32_t    kRgbaBytes           = 4U;
    constexpr std::uint8_t     kBaseRed             = 30U;
    constexpr std::uint8_t     kBaseGreen           = 80U;
    constexpr std::uint8_t     kBaseBlue            = 150U;
    constexpr std::uint8_t     kBaseAlpha           = 255U;
    constexpr std::uint8_t     kChangedRed          = 210U;
    constexpr std::uint8_t     kChangedGreen        = 20U;
    constexpr std::uint8_t     kChangedBlue         = 90U;
    constexpr std::uint8_t     kChangedAlpha        = 255U;
    constexpr std::size_t      kRedOffset           = 0U;
    constexpr std::size_t      kGreenOffset         = 1U;
    constexpr std::size_t      kBlueOffset          = 2U;
    constexpr std::size_t      kAlphaOffset         = 3U;
    constexpr std::size_t      kFirstWindowCount    = 1U;
    constexpr std::size_t      kTwoWindowCount      = 2U;
    constexpr auto             kPaintDelay          = std::chrono::milliseconds{ 50 };
    constexpr auto             kWatchWarmupDelay    = std::chrono::milliseconds{ 250 };
    constexpr auto             kWatchPollDelay      = std::chrono::milliseconds{ 20 };
    constexpr auto             kWatchTimeout        = std::chrono::seconds{ 4 };
    constexpr std::string_view kFirstInstance       = "grab-workflow-one-instance";
    constexpr std::string_view kFirstClass          = "GrabWorkflowOneClass";
    constexpr std::string_view kSecondInstance      = "grab-workflow-two-instance";
    constexpr std::string_view kSecondClass         = "GrabWorkflowTwoClass";
    constexpr std::string_view kMissingClass        = "GrabWorkflowMissingClass";
    constexpr std::string_view kWatchInstance       = "grab-workflow-watch-instance";
    constexpr std::string_view kWatchClass          = "GrabWorkflowWatchClass";
    constexpr std::string_view kInitialTitle        = "grab workflow initial title";
    constexpr std::string_view kChangedTitle        = "grab workflow changed title";
    constexpr std::string_view kNetClientListAtom   = "_NET_CLIENT_LIST";
    constexpr std::string_view kNetActiveWindowAtom = "_NET_ACTIVE_WINDOW";
    constexpr std::string_view kNetWmNameAtom       = "_NET_WM_NAME";
    constexpr std::string_view kUtf8StringAtom      = "UTF8_STRING";
    constexpr std::string_view kTempDirPrefix       = "grab-workflow-";

    template<typename T>
    using XcbOwned = std::unique_ptr<T, decltype( &std::free )>;

    template<typename T>
    [[nodiscard]]
    XcbOwned<T>
    take_xcb_owned( T* pointer ) noexcept
    {
        return XcbOwned<T>{ pointer, &std::free };
    }

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
                return std::string{ kTempDirPrefix } + std::to_string( now.count() );
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
            int               screen_index_ = kInvalidScreenIndex;
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
            xcb_connection_has_error( connection ) != kXcbOk )
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
    set_wm_class( xcb_connection_t* connection,
                  xcb_window_t      window,
                  std::string_view  instance,
                  std::string_view  class_name )
    {
        const std::string value = wm_class_value( instance, class_name );
        EXPECT_TRUE( request_succeeded(
            connection,
            xcb_change_property_checked( connection,
                                         kPropertyReplaceMode,
                                         window,
                                         XCB_ATOM_WM_CLASS,
                                         XCB_ATOM_STRING,
                                         kFormat8Bits,
                                         static_cast<std::uint32_t>( value.size() ),
                                         value.data() )
        ) );
    }

    void
    set_title( xcb_connection_t* connection,
               xcb_window_t      window,
               std::string_view  title )
    {
        xcb_atom_t net_wm_name = XCB_ATOM_NONE;
        xcb_atom_t utf8_string = XCB_ATOM_NONE;
        ASSERT_TRUE( intern_atom( connection, kNetWmNameAtom, net_wm_name ) );
        ASSERT_TRUE( intern_atom( connection, kUtf8StringAtom, utf8_string ) );
        EXPECT_TRUE( request_succeeded(
            connection,
            xcb_change_property_checked( connection,
                                         kPropertyReplaceMode,
                                         window,
                                         net_wm_name,
                                         utf8_string,
                                         kFormat8Bits,
                                         static_cast<std::uint32_t>( title.size() ),
                                         title.data() )
        ) );
    }

    void
    set_client_list( xcb_connection_t*             connection,
                     xcb_window_t                  root,
                     std::span<const xcb_window_t> windows )
    {
        xcb_atom_t net_client_list = XCB_ATOM_NONE;
        ASSERT_TRUE( intern_atom( connection, kNetClientListAtom, net_client_list ) );
        EXPECT_TRUE( request_succeeded(
            connection,
            xcb_change_property_checked( connection,
                                         kPropertyReplaceMode,
                                         root,
                                         net_client_list,
                                         XCB_ATOM_WINDOW,
                                         kFormat32Bits,
                                         static_cast<std::uint32_t>( windows.size() ),
                                         windows.data() )
        ) );
    }

    void
    set_active_window( xcb_connection_t* connection,
                       xcb_window_t      root,
                       xcb_window_t      window )
    {
        xcb_atom_t active_window_atom = XCB_ATOM_NONE;
        ASSERT_TRUE(
            intern_atom( connection, kNetActiveWindowAtom, active_window_atom )
        );
        EXPECT_TRUE( request_succeeded(
            connection,
            xcb_change_property_checked( connection,
                                         kPropertyReplaceMode,
                                         root,
                                         active_window_atom,
                                         XCB_ATOM_WINDOW,
                                         kFormat32Bits,
                                         static_cast<std::uint32_t>( kFirstWindowCount ),
                                         &window )
        ) );
    }

    [[nodiscard]]
    xcb_window_t
    create_solid_window( xcb_connection_t*   connection,
                         const xcb_screen_t& screen,
                         std::int16_t        x,
                         std::int16_t        y,
                         std::uint32_t       color,
                         std::string_view    instance,
                         std::string_view    class_name,
                         std::string_view    title )
    {
        const xcb_window_t window = xcb_generate_id( connection );
        const std::array<std::uint32_t, kFirstWindowCount> values{ color };
        EXPECT_TRUE(
            request_succeeded( connection,
                               xcb_create_window_checked( connection,
                                                          screen.root_depth,
                                                          window,
                                                          screen.root,
                                                          x,
                                                          y,
                                                          kWindowWidth,
                                                          kWindowHeight,
                                                          kWindowBorderWidth,
                                                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                                          screen.root_visual,
                                                          kWindowValueMask,
                                                          values.data() ) )
        );
        set_wm_class( connection, window, instance, class_name );
        set_title( connection, window, title );
        EXPECT_TRUE( request_succeeded( connection,
                                        xcb_map_window_checked( connection, window ) ) );
        EXPECT_TRUE( flush_succeeded( connection ) );
        std::this_thread::sleep_for( kPaintDelay );
        return window;
    }

    [[nodiscard]]
    std::byte
    byte_from( std::uint8_t value ) noexcept
    {
        return static_cast<std::byte>( value );
    }

    [[nodiscard]]
    std::size_t
    pixel_offset( const grab::Image& image,
                  std::uint32_t      x,
                  std::uint32_t      y ) noexcept
    {
        return ( static_cast<std::size_t>( y ) *
                 static_cast<std::size_t>( image.stride ) ) +
               ( static_cast<std::size_t>( x ) *
                 static_cast<std::size_t>( kRgbaBytes ) );
    }

    void
    set_pixel( grab::Image&  image,
               std::uint32_t x,
               std::uint32_t y,
               std::uint8_t  red,
               std::uint8_t  green,
               std::uint8_t  blue,
               std::uint8_t  alpha )
    {
        const auto offset                        = pixel_offset( image, x, y );
        image.pixels.at( offset + kRedOffset )   = byte_from( red );
        image.pixels.at( offset + kGreenOffset ) = byte_from( green );
        image.pixels.at( offset + kBlueOffset )  = byte_from( blue );
        image.pixels.at( offset + kAlphaOffset ) = byte_from( alpha );
    }

    [[nodiscard]]
    grab::Image
    make_rgba_image( std::uint8_t red,
                     std::uint8_t green,
                     std::uint8_t blue,
                     std::uint8_t alpha )
    {
        constexpr std::uint32_t kStride = kImageWidth * kRgbaBytes;
        grab::Image             image{
            .width  = kImageWidth,
            .height = kImageHeight,
            .stride = kStride,
            .format = grab::PixelFormat::rgba,
            .pixels = std::vector<std::byte>( static_cast<std::size_t>( kStride ) *
                                              static_cast<std::size_t>( kImageHeight ) ),
        };

        for( std::uint32_t y = 0U; y < kImageHeight; ++y )
        {
            for( std::uint32_t x = 0U; x < kImageWidth; ++x )
            {
                set_pixel( image, x, y, red, green, blue, alpha );
            }
        }
        return image;
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

        std::vector<char> bytes;
        bytes.reserve( encoded->size() );
        for( const std::byte value : *encoded )
        {
            bytes.push_back(
                static_cast<char>( std::to_integer<unsigned char>( value ) )
            );
        }

        std::ofstream stream{ path, std::ios::binary };
        if( !stream )
        {
            return grab::fail( grab::ErrorCode::provider_failed,
                               "failed to open test PNG: " + path.string() );
        }
        if( !bytes.empty() )
        {
            stream.write( bytes.data(), static_cast<std::streamsize>( bytes.size() ) );
        }
        if( !stream )
        {
            return grab::fail( grab::ErrorCode::provider_failed,
                               "failed to write test PNG: " + path.string() );
        }
        return {};
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

    [[nodiscard]]
    bool
    wait_for_file( const std::filesystem::path& path )
    {
        const auto deadline = std::chrono::steady_clock::now() + kWatchTimeout;
        while( std::chrono::steady_clock::now() < deadline )
        {
            if( std::filesystem::exists( path ) )
            {
                return true;
            }
            std::this_thread::sleep_for( kWatchPollDelay );
        }
        return std::filesystem::exists( path );
    }

}    // namespace

TEST( Workflow,
      BatchCapturesMultipleWindows )
{
    const TestConnection connection{ kXvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), kXcbOk );
    const xcb_screen_t* screen_info =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen_info, nullptr );

    const xcb_window_t first  = create_solid_window( connection.get(),
                                                     *screen_info,
                                                     kFirstWindowX,
                                                     kFirstWindowY,
                                                     kFirstColor,
                                                     kFirstInstance,
                                                     kFirstClass,
                                                     kInitialTitle );
    const xcb_window_t second = create_solid_window( connection.get(),
                                                     *screen_info,
                                                     kSecondWindowX,
                                                     kSecondWindowY,
                                                     kSecondColor,
                                                     kSecondInstance,
                                                     kSecondClass,
                                                     kInitialTitle );
    const std::array<xcb_window_t, kTwoWindowCount> windows{ first, second };
    set_client_list( connection.get(), screen_info->root, windows );
    EXPECT_TRUE( flush_succeeded( connection.get() ) );

    const TempDirectory temp;
    const auto          first_path   = temp.file( "first.png" );
    const auto          second_path  = temp.file( "second.png" );
    const auto          missing_path = temp.file( "missing.png" );

    auto                screen       = grab::Screen::open( kXvfbDisplay );
    ASSERT_TRUE( screen.has_value() ) << screen.error().message;

    const std::vector<grab::screen::BatchItem> items{
        grab::screen::BatchItem{
                                .wm_class_candidates = { std::string{ kFirstClass } },
                                .out_path            = first_path.string(),
                                },
        grab::screen::BatchItem{
                                .wm_class_candidates = { std::string{ kSecondClass } },
                                .out_path            = second_path.string(),
                                },
        grab::screen::BatchItem{
                                .wm_class_candidates = { std::string{ kMissingClass } },
                                .out_path            = missing_path.string(),
                                },
    };

    auto result = grab::screen::batch_capture( *screen, items );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    EXPECT_EQ( result->captured, kExpectedCaptured );
    ASSERT_EQ( result->misses.size(), kFirstWindowCount );
    EXPECT_EQ( result->misses.front(), kMissingClass );
    EXPECT_TRUE( std::filesystem::exists( first_path ) );
    EXPECT_TRUE( std::filesystem::exists( second_path ) );
    EXPECT_FALSE( std::filesystem::exists( missing_path ) );

    auto first_decoded = grab::codec::decode_png( read_file( first_path ) );
    ASSERT_TRUE( first_decoded.has_value() ) << first_decoded.error().message;
    EXPECT_EQ( first_decoded->width, kWindowWidth );
    EXPECT_EQ( first_decoded->height, kWindowHeight );

    auto second_decoded = grab::codec::decode_png( read_file( second_path ) );
    ASSERT_TRUE( second_decoded.has_value() ) << second_decoded.error().message;
    EXPECT_EQ( second_decoded->width, kWindowWidth );
    EXPECT_EQ( second_decoded->height, kWindowHeight );
}

TEST( Workflow,
      CompareFilesDetectsDifference )
{
    const TempDirectory temp;
    const auto          first_path     = temp.file( "first.png" );
    const auto          second_path    = temp.file( "second.png" );
    const auto          identical_path = temp.file( "identical.png" );

    const auto base = make_rgba_image( kBaseRed, kBaseGreen, kBaseBlue, kBaseAlpha );
    const auto changed =
        make_rgba_image( kChangedRed, kChangedGreen, kChangedBlue, kChangedAlpha );
    ASSERT_TRUE( write_png_file( first_path, base ).has_value() );
    ASSERT_TRUE( write_png_file( second_path, changed ).has_value() );
    ASSERT_TRUE( write_png_file( identical_path, base ).has_value() );

    auto different =
        grab::screen::compare_files( first_path.string(), second_path.string() );

    ASSERT_TRUE( different.has_value() ) << different.error().message;
    EXPECT_LT( different->match_ratio, kFullMatchRatio );
    EXPECT_GT( different->diff_pixels, kNoDiffPixels );

    auto identical =
        grab::screen::compare_files( first_path.string(), identical_path.string() );

    ASSERT_TRUE( identical.has_value() ) << identical.error().message;
    EXPECT_DOUBLE_EQ( identical->match_ratio, kFullMatchRatio );
    EXPECT_EQ( identical->diff_pixels, kNoDiffPixels );
}

TEST( Workflow,
      WatchCapturesOnTitleChange )
{
    const TestConnection connection{ kXvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), kXcbOk );
    const xcb_screen_t* screen_info =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen_info, nullptr );

    const xcb_window_t window = create_solid_window( connection.get(),
                                                     *screen_info,
                                                     kFirstWindowX,
                                                     kFirstWindowY,
                                                     kFirstColor,
                                                     kWatchInstance,
                                                     kWatchClass,
                                                     kInitialTitle );
    const std::array<xcb_window_t, kFirstWindowCount> windows{ window };
    set_client_list( connection.get(), screen_info->root, windows );
    set_active_window( connection.get(), screen_info->root, window );
    EXPECT_TRUE( flush_succeeded( connection.get() ) );

    const TempDirectory temp;
    const auto          out_path = temp.file( "watch.png" );
    auto                screen   = grab::Screen::open( kXvfbDisplay );
    ASSERT_TRUE( screen.has_value() ) << screen.error().message;

    std::atomic_bool should_stop{ false };
    auto             future =
        std::async( std::launch::async,
                    [&screen, &should_stop, &out_path]
                    {
                        return grab::screen::watch_capture(
                            *screen,
                            std::vector<std::string>{ std::string{ kWatchClass } },
                            out_path.string(),
                            [&should_stop]
                            {
                                return should_stop.load();
                            }
                        );
                    } );

    std::this_thread::sleep_for( kWatchWarmupDelay );
    set_title( connection.get(), window, kChangedTitle );
    EXPECT_TRUE( flush_succeeded( connection.get() ) );
    const bool captured_file_exists = wait_for_file( out_path );
    should_stop.store( true );

    auto result = future.get();

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    EXPECT_GE( *result, kMinimumWatchCapture );
    EXPECT_TRUE( captured_file_exists );
}
