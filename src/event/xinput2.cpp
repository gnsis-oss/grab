#include "core/reactor.hpp"
#include "event/xinput2.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <sys/epoll.h>    // IWYU pragma: keep
#include <utility>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/xinput.h>
#include <xcb/xproto.h>

namespace grab::event
{
    namespace
    {

        constexpr int           kInvalidFd                 = -1;
        constexpr int           kXcbOk                     = 0;
        constexpr int           kFlushFailed               = 0;
        constexpr std::uint64_t kNoToken                   = 0U;
        constexpr std::uint8_t  kResponseTypeMask          = 0X7FU;
        constexpr std::uint16_t kRequiredXiMajorVersion    = 2U;
        constexpr std::uint16_t kRequiredXiMinorVersion    = 0U;
        constexpr std::uint16_t kRawMaskCount              = 1U;
        constexpr std::uint16_t kRawMaskWords              = 1U;
        constexpr int           kXAxisValuator             = 0;
        constexpr int           kYAxisValuator             = 1;
        constexpr int           kBitsPerMaskWord           = 32;
        constexpr double        kFp3232FractionDenominator = 4'294'967'296.0;
        constexpr std::uint32_t kNoEvents                  = 0U;
        constexpr std::uint32_t kReadableEvents =
            static_cast<std::uint32_t>( EPOLLIN ) |
            static_cast<std::uint32_t>( EPOLLERR ) |
            static_cast<std::uint32_t>( EPOLLHUP );
        constexpr std::uint32_t kRawEventMask =
            static_cast<std::uint32_t>( XCB_INPUT_XI_EVENT_MASK_RAW_KEY_PRESS ) |
            static_cast<std::uint32_t>( XCB_INPUT_XI_EVENT_MASK_RAW_KEY_RELEASE ) |
            static_cast<std::uint32_t>( XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_PRESS ) |
            static_cast<std::uint32_t>( XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_RELEASE ) |
            static_cast<std::uint32_t>( XCB_INPUT_XI_EVENT_MASK_RAW_MOTION );

        template<typename T>
        using XcbOwned = std::unique_ptr<T, decltype( &std::free )>;

        using XcbConnection =
            std::unique_ptr<xcb_connection_t, decltype( &xcb_disconnect )>;

        struct RawEventSelection
        {
                xcb_input_event_mask_t                   event_mask;
                std::array<std::uint32_t, kRawMaskWords> mask_words;
        };

        template<typename T>
        [[nodiscard]]
        XcbOwned<T>
        take_xcb_owned( T* pointer ) noexcept
        {
            return XcbOwned<T>{ pointer, &std::free };
        }

        [[nodiscard]]
        grab::Result<std::uint32_t>
        default_root( xcb_connection_t* connection,
                      int               screen_index )
        {
            xcb_screen_iterator_t iterator =
                xcb_setup_roots_iterator( xcb_get_setup( connection ) );
            for( int current_screen = 0;
                 current_screen < screen_index && iterator.rem > 0;
                 ++current_screen )
            {
                xcb_screen_next( &iterator );
            }

            if( iterator.data == nullptr )
            {
                return grab::fail( grab::ErrorCode::device_inaccessible,
                                   "XCB default screen is unavailable" );
            }

            return iterator.data->root;
        }

        [[nodiscard]]
        grab::Result<std::uint8_t>
        require_xinput_extension( xcb_connection_t* connection )
        {
            const xcb_query_extension_reply_t* const extension =
                xcb_get_extension_data( connection, &xcb_input_id );
            if( extension == nullptr || extension->present == 0U )
            {
                return grab::fail( grab::ErrorCode::device_inaccessible,
                                   "XInput extension is unavailable" );
            }

            return extension->major_opcode;
        }

        [[nodiscard]]
        bool
        supports_required_xi_version(
            const xcb_input_xi_query_version_reply_t& version
        ) noexcept
        {
            return version.major_version >
                   kRequiredXiMajorVersion ||
                   ( version.major_version ==
                     kRequiredXiMajorVersion &&
                     version.minor_version >= kRequiredXiMinorVersion );
        }

        [[nodiscard]]
        grab::Result<void>
        require_xi2( xcb_connection_t* connection )
        {
            xcb_generic_error_t* raw_error = nullptr;
            const auto reply = take_xcb_owned( xcb_input_xi_query_version_reply(
                connection,
                xcb_input_xi_query_version( connection,
                                            kRequiredXiMajorVersion,
                                            kRequiredXiMinorVersion ),
                &raw_error
            ) );
            const auto error = take_xcb_owned( raw_error );

            if( error != nullptr || reply == nullptr )
            {
                return grab::fail( grab::ErrorCode::device_inaccessible,
                                   "XInput2 version query failed" );
            }

            if( !supports_required_xi_version( *reply ) )
            {
                return grab::fail( grab::ErrorCode::device_inaccessible,
                                   "XInput2 2.0 is unavailable" );
            }

            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        check_request( xcb_connection_t* connection,
                       xcb_void_cookie_t cookie,
                       std::string_view  operation )
        {
            const auto error = take_xcb_owned( xcb_request_check( connection, cookie ) );
            if( error != nullptr )
            {
                return grab::fail( grab::ErrorCode::protocol_error,
                                   std::string{ operation } +
                                       " failed with X error " +
                                       std::to_string( error->error_code ) );
            }

            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        select_raw_events( xcb_connection_t* connection,
                           std::uint32_t     root )
        {
            const auto selection = []
            {
                RawEventSelection value{};
                value.event_mask.deviceid =
                    static_cast<xcb_input_device_id_t>( XCB_INPUT_DEVICE_ALL_MASTER );
                value.event_mask.mask_len = kRawMaskWords;
                value.mask_words          = { kRawEventMask };
                return value;
            }();

            auto selected = check_request(
                connection,
                xcb_input_xi_select_events_checked( connection,
                                                    root,
                                                    kRawMaskCount,
                                                    &selection.event_mask ),
                "XISelectEvents raw input"
            );
            if( !selected.has_value() )
            {
                return selected;
            }

            if( xcb_flush( connection ) <=
                kFlushFailed ||
                xcb_connection_has_error( connection ) != kXcbOk )
            {
                return grab::fail( grab::ErrorCode::device_inaccessible,
                                   "XInput2 raw event selection flush failed" );
            }

            return {};
        }

        [[nodiscard]]
        double
        fp3232_to_double( xcb_input_fp3232_t value ) noexcept
        {
            return static_cast<double>( value.integral ) +
                   ( static_cast<double>( value.frac ) / kFp3232FractionDenominator );
        }

        [[nodiscard]]
        grab::Event
        make_key_event( grab::EventKind                        kind,
                        const xcb_input_raw_key_press_event_t& raw )
        {
            return grab::Event{
                .timestamp = static_cast<double>( raw.time ),
                .sequence  = 0U,
                .kind      = kind,
                .category  = grab::EventCategory::input,
                .payload   = grab::Payload{ grab::InputKey{
                    .code = raw.detail,
                    .name = {},
                } },
            };
        }

        [[nodiscard]]
        grab::Event
        make_button_event( const xcb_input_raw_button_press_event_t& raw )
        {
            return grab::Event{
                .timestamp = static_cast<double>( raw.time ),
                .sequence  = 0U,
                .kind      = grab::EventKind::mouse_click,
                .category  = grab::EventCategory::input,
                .payload   = grab::Payload{ grab::MouseClick{
                    .button = raw.detail,
                    .name   = {},
                } },
            };
        }

        [[nodiscard]]
        grab::Event
        make_motion_event( double           timestamp,
                           std::string_view axis,
                           double           delta )
        {
            return grab::Event{
                .timestamp = timestamp,
                .sequence  = 0U,
                .kind      = grab::EventKind::mouse_move,
                .category  = grab::EventCategory::input,
                .payload   = grab::Payload{ grab::MouseMove{
                    .axis  = std::string{ axis },
                    .delta = delta,
                } },
            };
        }

        void
        append_motion_axis( std::vector<grab::Event>& events,
                            double                    timestamp,
                            int                       axis,
                            double                    delta )
        {
            if( axis == kXAxisValuator )
            {
                events.push_back( make_motion_event( timestamp, "x", delta ) );
                return;
            }

            if( axis == kYAxisValuator )
            {
                events.push_back( make_motion_event( timestamp, "y", delta ) );
            }
        }

        void
        append_motion_events( const xcb_input_raw_motion_event_t& raw,
                              std::vector<grab::Event>&           events )
        {
            const int mask_length =
                xcb_input_raw_button_press_valuator_mask_length( &raw );
            const int values_length =
                xcb_input_raw_button_press_axisvalues_raw_length( &raw );
            const std::uint32_t* const mask =
                xcb_input_raw_button_press_valuator_mask( &raw );
            const xcb_input_fp3232_t* const values =
                xcb_input_raw_button_press_axisvalues_raw( &raw );
            const auto timestamp = static_cast<double>( raw.time );

            if( mask ==
                nullptr ||
                values ==
                nullptr ||
                mask_length <=
                0 ||
                values_length <= 0 )
            {
                events.push_back( make_motion_event( timestamp, "motion", 0.0 ) );
                return;
            }

            const std::span<const std::uint32_t> mask_words{
                mask,
                static_cast<std::size_t>( mask_length ),
            };
            const std::span<const xcb_input_fp3232_t> values_span{
                values,
                static_cast<std::size_t>( values_length ),
            };
            const auto size_before = events.size();
            int        value_index = 0;
            for( int word_index = 0;
                 word_index < mask_length && value_index < values_length;
                 ++word_index )
            {
                const std::uint32_t word = *std::next( mask_words.begin(), word_index );
                for( int bit_index = 0;
                     bit_index < kBitsPerMaskWord && value_index < values_length;
                     ++bit_index )
                {
                    const std::uint32_t bit = static_cast<std::uint32_t>( 1U )
                                           << bit_index;
                    if( ( word & bit ) == kNoEvents )
                    {
                        continue;
                    }

                    const int axis = ( word_index * kBitsPerMaskWord ) + bit_index;
                    append_motion_axis(
                        events,
                        timestamp,
                        axis,
                        fp3232_to_double( *std::next( values_span.begin(),
                                                      value_index ) )
                    );
                    ++value_index;
                }
            }

            if( events.size() == size_before )
            {
                events.push_back( make_motion_event( timestamp, "motion", 0.0 ) );
            }
        }

        void
        append_decoded_event( const xcb_generic_event_t& raw_event,
                              std::uint8_t               extension_opcode,
                              std::vector<grab::Event>&  events )
        {
            const auto response_type =
                static_cast<std::uint8_t>( raw_event.response_type & kResponseTypeMask );
            if( response_type != XCB_GE_GENERIC )
            {
                return;
            }

            const void* const event_storage = &raw_event;
            const auto* const generic =
                static_cast<const xcb_ge_generic_event_t*>( event_storage );
            if( generic->extension != extension_opcode )
            {
                return;
            }

            switch( generic->event_type )
            {
                case XCB_INPUT_RAW_KEY_PRESS :
                    {
                        const auto* const raw =
                            static_cast<const xcb_input_raw_key_press_event_t*>(
                                event_storage
                            );
                        events.push_back( make_key_event( grab::EventKind::key_down,
                                                          *raw ) );
                        break;
                    }
                case XCB_INPUT_RAW_KEY_RELEASE :
                    {
                        const auto* const raw =
                            static_cast<const xcb_input_raw_key_release_event_t*>(
                                event_storage
                            );
                        events.push_back( make_key_event( grab::EventKind::key_up,
                                                          *raw ) );
                        break;
                    }
                case XCB_INPUT_RAW_BUTTON_PRESS :
                    {
                        const auto* const raw =
                            static_cast<const xcb_input_raw_button_press_event_t*>(
                                event_storage
                            );
                        events.push_back( make_button_event( *raw ) );
                        break;
                    }
                case XCB_INPUT_RAW_BUTTON_RELEASE :
                    break;
                case XCB_INPUT_RAW_MOTION :
                    {
                        const auto* const raw =
                            static_cast<const xcb_input_raw_motion_event_t*>(
                                event_storage
                            );
                        append_motion_events( *raw, events );
                        break;
                    }
                default :
                    break;
            }
        }

        void
        publish_pending_events( grab::EventBus&           bus,
                                std::vector<grab::Event>& events ) noexcept
        {
            for( auto& event : events )
            {
                bus.publish( std::move( event ) );
            }
        }

    }    // namespace

    struct XInput2Monitor::State
    {
            std::mutex        mutex;
            xcb_connection_t* connection       = nullptr;
            grab::EventBus*   bus              = nullptr;
            std::uint8_t      extension_opcode = 0U;
            bool              active           = true;
    };

    XInput2Monitor::XInput2Monitor( grab::core::Reactor&   reactor,
                                    std::uint64_t          token,
                                    std::shared_ptr<State> state ) noexcept :
        reactor_( &reactor ),
        token_( token ),
        state_( std::move( state ) )
    {
    }

    XInput2Monitor::~XInput2Monitor()
    {
        stop();
    }

    XInput2Monitor::XInput2Monitor( XInput2Monitor&& other ) noexcept :
        reactor_( std::exchange( other.reactor_,
                                 nullptr ) ),
        token_( std::exchange( other.token_,
                               kNoToken ) ),
        state_( std::move( other.state_ ) )
    {
    }

    XInput2Monitor&
    XInput2Monitor::operator=( XInput2Monitor&& other ) noexcept
    {
        if( this != &other )
        {
            stop();
            reactor_ = std::exchange( other.reactor_, nullptr );
            token_   = std::exchange( other.token_, kNoToken );
            state_   = std::move( other.state_ );
        }
        return *this;
    }

    grab::Result<XInput2Monitor>
    XInput2Monitor::start( const char*          display,
                           grab::core::Reactor& reactor,
                           grab::EventBus&      bus )
    {
        int           screen_index = 0;
        XcbConnection connection{
            xcb_connect( display, &screen_index ),
            &xcb_disconnect
        };
        if( connection ==
            nullptr ||
            xcb_connection_has_error( connection.get() ) != kXcbOk )
        {
            return grab::fail( grab::ErrorCode::device_inaccessible,
                               "XCB display connection failed" );
        }

        auto extension_opcode = require_xinput_extension( connection.get() );
        if( !extension_opcode.has_value() )
        {
            return std::unexpected( std::move( extension_opcode.error() ) );
        }

        auto version = require_xi2( connection.get() );
        if( !version.has_value() )
        {
            return std::unexpected( std::move( version.error() ) );
        }

        auto root = default_root( connection.get(), screen_index );
        if( !root.has_value() )
        {
            return std::unexpected( std::move( root.error() ) );
        }

        auto selected = select_raw_events( connection.get(), *root );
        if( !selected.has_value() )
        {
            return std::unexpected( std::move( selected.error() ) );
        }

        const int fd = xcb_get_file_descriptor( connection.get() );
        if( fd == kInvalidFd )
        {
            return grab::fail( grab::ErrorCode::device_inaccessible,
                               "XCB connection file descriptor is unavailable" );
        }

        auto state              = std::make_shared<State>();
        state->connection       = connection.release();
        state->bus              = &bus;
        state->extension_opcode = *extension_opcode;

        const auto token =
            reactor.add_fd( fd,
                            static_cast<std::uint32_t>( EPOLLIN ),
                            [state]( std::uint32_t event_mask )
                            {
                                XInput2Monitor::handle_fd( state, event_mask );
                            } );

        return XInput2Monitor{ reactor, token, std::move( state ) };
    }

    void
    XInput2Monitor::handle_fd( const std::shared_ptr<State>& state,
                               std::uint32_t                 events )
    {
        if( ( events & kReadableEvents ) == kNoEvents )
        {
            return;
        }

        std::vector<grab::Event> pending_events;
        grab::EventBus*          bus = nullptr;
        {
            const std::scoped_lock lock( state->mutex );
            if( !state->active || state->connection == nullptr || state->bus == nullptr )
            {
                return;
            }

            bus = state->bus;
            while( true )
            {
                const auto event =
                    take_xcb_owned( xcb_poll_for_event( state->connection ) );
                if( event == nullptr )
                {
                    break;
                }

                append_decoded_event( *event, state->extension_opcode, pending_events );
            }

            if( xcb_connection_has_error( state->connection ) != kXcbOk )
            {
                state->active = false;
            }
        }

        if( bus != nullptr )
        {
            publish_pending_events( *bus, pending_events );
        }
    }

    void
    XInput2Monitor::stop() noexcept
    {
        grab::core::Reactor* const reactor = std::exchange( reactor_, nullptr );
        const std::uint64_t        token   = std::exchange( token_, kNoToken );
        auto                       state   = std::move( state_ );

        if( reactor != nullptr && token != kNoToken )
        {
            bool remove_failed = false;
            try
            {
                reactor->remove_fd( token );
            }
            catch( ... )
            {
                remove_failed = true;
            }
            static_cast<void>( remove_failed );
        }

        if( state == nullptr )
        {
            return;
        }

        xcb_connection_t* connection = nullptr;
        try
        {
            const std::scoped_lock lock( state->mutex );
            state->active = false;
            state->bus    = nullptr;
            connection    = std::exchange( state->connection, nullptr );
        }
        catch( ... )
        {
            return;
        }

        if( connection != nullptr )
        {
            xcb_disconnect( connection );
        }
    }

}    // namespace grab::event
