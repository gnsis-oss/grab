#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

namespace
{

    constexpr int              failureExitCode      = 1;
    constexpr int              initialScreenIndex   = 0;
    constexpr int              noRemainingScreens   = 0;
    constexpr int              minimumValidPid      = 0;
    constexpr int              xcbSuccess           = 0;
    constexpr int              xcbFlushFailure      = 0;
    constexpr std::int16_t     windowX              = 48;
    constexpr std::int16_t     windowY              = 56;
    constexpr std::uint16_t    windowWidth          = 240U;
    constexpr std::uint16_t    windowHeight         = 160U;
    constexpr std::uint16_t    windowBorderWidth    = 0U;
    constexpr std::uint32_t    windowValueMask      = XCB_CW_BACK_PIXEL;
    constexpr std::uint32_t    windowBackground     = 0X00'20'60'A0U;
    constexpr std::size_t      windowValueCount     = 1U;
    constexpr std::uint8_t     propertyReplaceMode  = XCB_PROP_MODE_REPLACE;
    constexpr std::uint8_t     propertyFormat8Bits  = 8U;
    constexpr std::uint8_t     propertyFormat32Bits = 32U;
    constexpr std::uint32_t    onePropertyItem      = 1U;
    constexpr std::uint8_t     createAtomIfMissing  = 0U;
    constexpr std::string_view windowInstance       = "grab-config-batch-window";
    constexpr std::string_view windowClass          = "GrabConfigBatchWindow";
    constexpr std::string_view windowTitle          = "grab config batch test window";
    constexpr std::string_view netWmPidAtomName     = "_NET_WM_PID";

    template<typename T>
    using XcbOwned = std::unique_ptr<T, decltype( &std::free )>;

    template<typename T>
    [[nodiscard]]
    XcbOwned<T>
    take_xcb_owned( T* pointer ) noexcept
    {
        return XcbOwned<T>{ pointer, &std::free };
    }

    class Connection
    {
        public:

            Connection() :
                value_( xcb_connect( nullptr,
                                     &screen_index_ ) )
            {
            }

            ~Connection()
            {
                if( value_ != nullptr )
                {
                    xcb_disconnect( value_ );
                }
            }

            Connection( const Connection& ) = delete;
            Connection&
            operator=( const Connection& ) = delete;
            Connection( Connection&& )     = delete;
            Connection&
            operator=( Connection&& ) = delete;

            [[nodiscard]]
            xcb_connection_t*
            get() const noexcept
            {
                return value_;
            }

            [[nodiscard]]
            int
            screen_index() const noexcept
            {
                return screen_index_;
            }

        private:

            xcb_connection_t* value_        = nullptr;
            int               screen_index_ = initialScreenIndex;
    };

    [[nodiscard]]
    const xcb_screen_t*
    default_screen( xcb_connection_t* connection,
                    int               screen_index )
    {
        xcb_screen_iterator_t iterator =
            xcb_setup_roots_iterator( xcb_get_setup( connection ) );
        for( int current = initialScreenIndex;
             current < screen_index && iterator.rem > noRemainingScreens;
             ++current )
        {
            xcb_screen_next( &iterator );
        }
        return iterator.data;
    }

    [[nodiscard]]
    bool
    request_succeeded( xcb_connection_t* connection,
                       xcb_void_cookie_t cookie )
    {
        return take_xcb_owned( xcb_request_check( connection, cookie ) ) == nullptr;
    }

    [[nodiscard]]
    bool
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
    xcb_atom_t
    intern_atom( xcb_connection_t* connection,
                 std::string_view  name )
    {
        if( name.size() > std::numeric_limits<std::uint16_t>::max() )
        {
            return XCB_ATOM_NONE;
        }
        xcb_generic_error_t* raw_error = nullptr;
        const auto           reply     = take_xcb_owned( xcb_intern_atom_reply(
            connection,
            xcb_intern_atom( connection,
                             createAtomIfMissing,
                             static_cast<std::uint16_t>( name.size() ),
                             name.data() ),
            &raw_error
        ) );
        const auto           error     = take_xcb_owned( raw_error );
        return error == nullptr && reply != nullptr ? reply->atom : XCB_ATOM_NONE;
    }

    [[nodiscard]]
    std::string
    wm_class_value()
    {
        std::string value{ windowInstance };
        value.push_back( '\0' );
        value.append( windowClass );
        value.push_back( '\0' );
        return value;
    }

    [[nodiscard]]
    bool
    set_window_properties( xcb_connection_t* connection,
                           xcb_window_t      window )
    {
        const std::string wm_class = wm_class_value();
        if( wm_class.size() >
            std::numeric_limits<std::uint32_t>::max() ||
            windowTitle.size() > std::numeric_limits<std::uint32_t>::max() )
        {
            return false;
        }
        if( !set_property( connection,
                           window,
                           XCB_ATOM_WM_CLASS,
                           XCB_ATOM_STRING,
                           propertyFormat8Bits,
                           static_cast<std::uint32_t>( wm_class.size() ),
                           wm_class.data() ) ||
            !set_property( connection,
                           window,
                           XCB_ATOM_WM_NAME,
                           XCB_ATOM_STRING,
                           propertyFormat8Bits,
                           static_cast<std::uint32_t>( windowTitle.size() ),
                           windowTitle.data() ) )
        {
            return false;
        }

        const auto pid = ::getpid();
        if( pid <=
            minimumValidPid ||
            static_cast<std::uint64_t>( pid ) >
            std::numeric_limits<std::uint32_t>::max() )
        {
            return false;
        }
        const std::uint32_t pid_value = static_cast<std::uint32_t>( pid );
        const xcb_atom_t    pid_atom  = intern_atom( connection, netWmPidAtomName );
        return pid_atom !=
               XCB_ATOM_NONE &&
               set_property( connection,
                             window,
                             pid_atom,
                             XCB_ATOM_CARDINAL,
                             propertyFormat32Bits,
                             onePropertyItem,
                             &pid_value );
    }

}    // namespace

int
main()
{
    Connection connection;
    if( connection.get() ==
        nullptr ||
        xcb_connection_has_error( connection.get() ) != xcbSuccess )
    {
        return failureExitCode;
    }

    const xcb_screen_t* screen =
        default_screen( connection.get(), connection.screen_index() );
    if( screen == nullptr )
    {
        return failureExitCode;
    }

    const xcb_window_t window = xcb_generate_id( connection.get() );
    const std::array<std::uint32_t, windowValueCount> values{ windowBackground };
    if( !request_succeeded( connection.get(),
                            xcb_create_window_checked( connection.get(),
                                                       screen->root_depth,
                                                       window,
                                                       screen->root,
                                                       windowX,
                                                       windowY,
                                                       windowWidth,
                                                       windowHeight,
                                                       windowBorderWidth,
                                                       XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                                       screen->root_visual,
                                                       windowValueMask,
                                                       values.data() ) ) ||
        !set_window_properties( connection.get(), window ) ||
        !request_succeeded( connection.get(),
                            xcb_map_window_checked( connection.get(), window ) ) ||
        xcb_flush( connection.get() ) <= xcbFlushFailure )
    {
        return failureExitCode;
    }

    for( ;; )
    {
        auto event = take_xcb_owned( xcb_wait_for_event( connection.get() ) );
        if( event == nullptr )
        {
            return failureExitCode;
        }
    }
}
