#include "grab/result.hpp"
#include "screen/enumerate.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
// clang-format on

namespace
{

    constexpr const char*      xvfbDisplay         = ":92";
    constexpr const char*      badDisplay          = ":bad-nonexistent-92";
    constexpr int              xcbOk               = 0;
    constexpr int              invalidScreenIndex  = 0;
    constexpr std::int16_t     firstWindowX        = 40;
    constexpr std::int16_t     firstWindowY        = 52;
    constexpr std::int16_t     secondWindowX       = 260;
    constexpr std::int16_t     secondWindowY       = 180;
    constexpr std::uint16_t    windowWidth         = 180U;
    constexpr std::uint16_t    windowHeight        = 120U;
    constexpr std::uint16_t    windowBorderWidth   = 0U;
    constexpr std::uint16_t    xvfbWidth           = 1'280U;
    constexpr std::uint16_t    xvfbHeight          = 1'024U;
    constexpr std::uint32_t    windowValueMask     = XCB_CW_BACK_PIXEL;
    constexpr std::uint32_t    propertyReplaceMode = XCB_PROP_MODE_REPLACE;
    constexpr std::uint8_t     format8Bits         = 8U;
    constexpr std::uint8_t     format32Bits        = 32U;
    constexpr std::size_t      createdWindowCount  = 2U;
    constexpr std::uint32_t    firstWindowColor    = 0X00'22'44'66U;
    constexpr std::uint32_t    secondWindowColor   = 0X00'66'44'22U;

    constexpr std::string_view firstInstance       = "grab-enumerate-one";
    constexpr std::string_view firstClass          = "GrabEnumerateOne";
    constexpr std::string_view firstTitle          = "grab enumerate first";
    constexpr std::string_view secondInstance      = "grab-enumerate-two";
    constexpr std::string_view secondClass         = "GrabEnumerateTwo";
    constexpr std::string_view secondTitle         = "grab enumerate second";
    constexpr std::string_view netClientListAtom   = "_NET_CLIENT_LIST";
    constexpr std::string_view netWmNameAtom       = "_NET_WM_NAME";
    constexpr std::string_view utf8StringAtom      = "UTF8_STRING";

    template<typename T>
    using XcbOwned = std::unique_ptr<T, decltype( &std::free )>;

    template<typename T>
    [[nodiscard]]
    XcbOwned<T>
    take_xcb_owned( T* pointer ) noexcept
    {
        return XcbOwned<T>{ pointer, &std::free };
    }

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
                                         propertyReplaceMode,
                                         window,
                                         XCB_ATOM_WM_CLASS,
                                         XCB_ATOM_STRING,
                                         format8Bits,
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
        ASSERT_TRUE( intern_atom( connection, netWmNameAtom, net_wm_name ) );
        ASSERT_TRUE( intern_atom( connection, utf8StringAtom, utf8_string ) );
        EXPECT_TRUE( request_succeeded(
            connection,
            xcb_change_property_checked( connection,
                                         propertyReplaceMode,
                                         window,
                                         net_wm_name,
                                         utf8_string,
                                         format8Bits,
                                         static_cast<std::uint32_t>( title.size() ),
                                         title.data() )
        ) );
    }

    void
    set_client_list( xcb_connection_t* connection,
                     xcb_window_t      root,
                     xcb_window_t      first_window,
                     xcb_window_t      second_window )
    {
        xcb_atom_t net_client_list = XCB_ATOM_NONE;
        ASSERT_TRUE( intern_atom( connection, netClientListAtom, net_client_list ) );
        const std::array<xcb_window_t, createdWindowCount> windows{
            first_window,
            second_window,
        };
        EXPECT_TRUE( request_succeeded(
            connection,
            xcb_change_property_checked( connection,
                                         propertyReplaceMode,
                                         root,
                                         net_client_list,
                                         XCB_ATOM_WINDOW,
                                         format32Bits,
                                         static_cast<std::uint32_t>( windows.size() ),
                                         windows.data() )
        ) );
    }

    [[nodiscard]]
    xcb_window_t
    create_test_window( xcb_connection_t*   connection,
                        const xcb_screen_t& screen,
                        std::int16_t        x,
                        std::int16_t        y,
                        std::uint32_t       color,
                        std::string_view    instance,
                        std::string_view    class_name,
                        std::string_view    title )
    {
        const xcb_window_t                 window = xcb_generate_id( connection );
        const std::array<std::uint32_t, 1> values{ color };
        EXPECT_TRUE(
            request_succeeded( connection,
                               xcb_create_window_checked( connection,
                                                          screen.root_depth,
                                                          window,
                                                          screen.root,
                                                          x,
                                                          y,
                                                          windowWidth,
                                                          windowHeight,
                                                          windowBorderWidth,
                                                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                                          screen.root_visual,
                                                          windowValueMask,
                                                          values.data() ) )
        );
        set_wm_class( connection, window, instance, class_name );
        set_title( connection, window, title );
        EXPECT_TRUE( request_succeeded( connection,
                                        xcb_map_window_checked( connection, window ) ) );
        EXPECT_TRUE( flush_succeeded( connection ) );
        return window;
    }

    [[nodiscard]]
    std::vector<grab::screen::WindowInfo>::const_iterator
    find_by_class( const std::vector<grab::screen::WindowInfo>& windows,
                   std::string_view                             wm_class )
    {
        return std::ranges::find_if( windows,
                                     [wm_class]( const grab::screen::WindowInfo& info )
                                     {
                                         return info.wm_class == wm_class;
                                     } );
    }

}    // namespace

TEST( Enumerate,
      ListsCreatedWindows )
{
    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen, nullptr );

    const xcb_window_t first_window  = create_test_window( connection.get(),
                                                           *screen,
                                                           firstWindowX,
                                                           firstWindowY,
                                                           firstWindowColor,
                                                           firstInstance,
                                                           firstClass,
                                                           firstTitle );
    const xcb_window_t second_window = create_test_window( connection.get(),
                                                           *screen,
                                                           secondWindowX,
                                                           secondWindowY,
                                                           secondWindowColor,
                                                           secondInstance,
                                                           secondClass,
                                                           secondTitle );
    set_client_list( connection.get(), screen->root, first_window, second_window );
    ASSERT_TRUE( flush_succeeded( connection.get() ) );

    auto listed = grab::screen::list_windows( xvfbDisplay );

    ASSERT_TRUE( listed.has_value() ) << listed.error().message;
    const auto first = find_by_class( *listed, firstClass );
    ASSERT_NE( first, listed->end() );
    EXPECT_EQ( first->id, static_cast<std::uint32_t>( first_window ) );
    EXPECT_EQ( first->title, firstTitle );
    EXPECT_EQ( first->bounds.x, firstWindowX );
    EXPECT_EQ( first->bounds.y, firstWindowY );
    EXPECT_EQ( first->bounds.width, windowWidth );
    EXPECT_EQ( first->bounds.height, windowHeight );

    const auto second = find_by_class( *listed, secondClass );
    ASSERT_NE( second, listed->end() );
    EXPECT_EQ( second->id, static_cast<std::uint32_t>( second_window ) );
    EXPECT_EQ( second->title, secondTitle );
    EXPECT_EQ( second->bounds.x, secondWindowX );
    EXPECT_EQ( second->bounds.y, secondWindowY );
    EXPECT_EQ( second->bounds.width, windowWidth );
    EXPECT_EQ( second->bounds.height, windowHeight );
}

TEST( Enumerate,
      ListsAtLeastOneOutput )
{
    auto outputs = grab::screen::list_outputs( xvfbDisplay );

    ASSERT_TRUE( outputs.has_value() ) << outputs.error().message;
    ASSERT_FALSE( outputs->empty() );
    const auto matching_screen =
        std::ranges::find_if( *outputs,
                              []( const grab::screen::OutputInfo& output )
                              {
                                  return output.bounds.width ==
                                         xvfbWidth &&
                                         output.bounds.height == xvfbHeight;
                              } );
    EXPECT_NE( matching_screen, outputs->end() );
}

TEST( Enumerate,
      ListWindowsFailsOnBadDisplay )
{
    auto windows = grab::screen::list_windows( badDisplay );

    ASSERT_FALSE( windows.has_value() );
    EXPECT_EQ( windows.error().code, grab::ErrorCode::DeviceInaccessible );
}
