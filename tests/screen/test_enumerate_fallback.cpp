#include "drivers/desktop/x11/enumerate.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
// clang-format on

namespace
{

    constexpr const char*      xvfbDisplay            = ":97";
    constexpr int              xcbOk                  = 0;
    constexpr int              invalidScreenIndex     = 0;
    constexpr std::int16_t     mappedWindowX          = 84;
    constexpr std::int16_t     mappedWindowY          = 96;
    constexpr std::int16_t     unmappedWindowX        = 312;
    constexpr std::int16_t     unmappedWindowY        = 224;
    constexpr std::uint16_t    windowWidth            = 192U;
    constexpr std::uint16_t    windowHeight           = 128U;
    constexpr std::uint16_t    windowBorderWidth      = 0U;
    constexpr std::uint32_t    windowValueMask        = XCB_CW_BACK_PIXEL;
    constexpr std::uint8_t     propertyReplaceMode    = XCB_PROP_MODE_REPLACE;
    constexpr std::uint32_t    mappedWindowColor      = 0X00'31'72'A4U;
    constexpr std::uint32_t    unmappedWindowColor    = 0X00'A4'72'31U;
    constexpr std::uint32_t    expectedPid            = 73'421U;
    constexpr std::uint32_t    onePropertyItem        = 1U;
    constexpr std::uint8_t     format8Bits            = 8U;
    constexpr std::uint8_t     format32Bits           = 32U;
    constexpr std::size_t      singleWindowValueCount = 1U;
    constexpr std::string_view mappedInstance         = "grab-fallback-mapped";
    constexpr std::string_view mappedClass            = "GrabFallbackMappedClass";
    constexpr std::string_view mappedTitle            = "grab fallback mapped title";
    constexpr std::string_view unmappedInstance       = "grab-fallback-unmapped";
    constexpr std::string_view unmappedClass          = "GrabFallbackUnmappedClass";
    constexpr std::string_view unmappedTitle          = "grab fallback unmapped title";
    constexpr std::string_view netClientListAtom      = "_NET_CLIENT_LIST";
    constexpr std::string_view netWmNameAtom          = "_NET_WM_NAME";
    constexpr std::string_view netWmPidAtom           = "_NET_WM_PID";
    constexpr std::string_view utf8StringAtom         = "UTF8_STRING";

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
                           xcb_window_t      window,
                           std::string_view  instance,
                           std::string_view  class_name,
                           std::string_view  title,
                           std::uint32_t     pid )
    {
        const std::string wm_class = wm_class_value( instance, class_name );
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
                                   static_cast<std::uint32_t>( title.size() ),
                                   title.data() ) );

        xcb_atom_t net_wm_pid = XCB_ATOM_NONE;
        ASSERT_TRUE( intern_atom( connection, netWmPidAtom, net_wm_pid ) );
        EXPECT_TRUE( set_property( connection,
                                   window,
                                   net_wm_pid,
                                   XCB_ATOM_CARDINAL,
                                   format32Bits,
                                   onePropertyItem,
                                   &pid ) );
    }

    [[nodiscard]]
    xcb_window_t
    create_window( xcb_connection_t*   connection,
                   const xcb_screen_t& screen,
                   std::int16_t        x,
                   std::int16_t        y,
                   std::uint32_t       color,
                   std::string_view    instance,
                   std::string_view    class_name,
                   std::string_view    title,
                   std::uint32_t       pid,
                   bool                mapped )
    {
        const xcb_window_t window = xcb_generate_id( connection );
        const std::array<std::uint32_t, singleWindowValueCount> values{ color };
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
        set_window_properties( connection, window, instance, class_name, title, pid );
        if( mapped )
        {
            EXPECT_TRUE( request_succeeded( connection,
                                            xcb_map_window_checked( connection,
                                                                    window ) ) );
        }
        return window;
    }

    void
    remove_client_list( xcb_connection_t* connection,
                        xcb_window_t      root )
    {
        xcb_atom_t net_client_list = XCB_ATOM_NONE;
        ASSERT_TRUE( intern_atom( connection, netClientListAtom, net_client_list ) );
        ASSERT_NE( net_client_list, XCB_ATOM_NONE );
        EXPECT_TRUE( request_succeeded(
            connection,
            xcb_delete_property_checked( connection, root, net_client_list )
        ) );
    }

}    // namespace

TEST( EnumerateFallback,
      ListsMappedWindowWithoutClientList )
{
    const TestConnection connection{ xvfbDisplay };
    ASSERT_NE( connection.get(), nullptr );
    ASSERT_EQ( xcb_connection_has_error( connection.get() ), xcbOk );
    const xcb_screen_t* screen =
        default_screen( connection.get(), connection.screen_index() );
    ASSERT_NE( screen, nullptr );
    remove_client_list( connection.get(), screen->root );

    const xcb_window_t mapped_window   = create_window( connection.get(),
                                                        *screen,
                                                        mappedWindowX,
                                                        mappedWindowY,
                                                        mappedWindowColor,
                                                        mappedInstance,
                                                        mappedClass,
                                                        mappedTitle,
                                                        expectedPid,
                                                        true );
    const xcb_window_t unmapped_window = create_window( connection.get(),
                                                        *screen,
                                                        unmappedWindowX,
                                                        unmappedWindowY,
                                                        unmappedWindowColor,
                                                        unmappedInstance,
                                                        unmappedClass,
                                                        unmappedTitle,
                                                        expectedPid,
                                                        false );
    ASSERT_TRUE( flush_succeeded( connection.get() ) );

    auto listed = grab::screen::list_windows( connection.get(), screen->root );

    ASSERT_TRUE( listed.has_value() ) << listed.error().message;
    const auto mapped = std::ranges::find( *listed,
                                           static_cast<std::uint32_t>( mapped_window ),
                                           &grab::screen::WindowInfo::id );
    ASSERT_NE( mapped, listed->end() );
    EXPECT_EQ( mapped->wm_class, mappedClass );
    EXPECT_EQ( mapped->title, mappedTitle );
    ASSERT_TRUE( mapped->pid.has_value() );
    EXPECT_EQ( *mapped->pid, expectedPid );

    const auto unmapped =
        std::ranges::find( *listed,
                           static_cast<std::uint32_t>( unmapped_window ),
                           &grab::screen::WindowInfo::id );
    EXPECT_EQ( unmapped, listed->end() );
}
