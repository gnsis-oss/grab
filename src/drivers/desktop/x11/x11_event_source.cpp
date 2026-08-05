#include "drivers/desktop/x11/injection_ledger.hpp"
#include "drivers/desktop/x11/x11_event_source.hpp"
#include "grab/context.hpp"
#include "grab/event.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/origin.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "kernel/events/wall_clock.hpp"
#include "spi/event_source.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/xinput.h>
#include <xcb/xproto.h>

namespace grab::drivers::desktop::x11
{
    namespace
    {

        constexpr int           invalidFd                 = -1;
        constexpr int           xcbOk                     = 0;
        constexpr int           flushFailed               = 0;
        constexpr std::uint8_t  responseTypeMask          = 0X7FU;
        constexpr std::uint16_t requiredXiMajorVersion    = 2U;
        constexpr std::uint16_t requiredXiMinorVersion    = 1U;
        constexpr std::uint16_t rawMaskCount              = 1U;
        constexpr std::uint16_t rawMaskWords              = 1U;
        constexpr int           xAxisValuator             = 0;
        constexpr int           yAxisValuator             = 1;
        constexpr int           bitsPerMaskWord           = 32;
        constexpr double        fp3232FractionDenominator = 4'294'967'296.0;
        constexpr std::uint32_t noEvents                  = 0U;
        constexpr std::uint8_t  differentScreen           = 0U;

        constexpr std::size_t   keyDownDemandSlot         = 0U;
        constexpr std::size_t   keyUpDemandSlot           = 1U;
        constexpr std::size_t   mouseClickDemandSlot      = 2U;
        constexpr std::size_t   mouseMoveDemandSlot       = 3U;
        constexpr std::size_t   mouseButtonDownDemandSlot = 4U;
        constexpr std::size_t   mouseButtonUpDemandSlot   = 5U;
        constexpr std::size_t   noDemand                  = 0U;
        constexpr std::uint32_t firstMaskBit              = 1U;
        constexpr std::uint64_t initialSequence           = 0U;
        constexpr int           noValuatorEntries         = 0;
        constexpr int           noDeviceInfos             = 0;
        constexpr int           noNameBytes               = 0;
        constexpr short         noPollEvents              = 0;
        constexpr nfds_t        pollTargetCount           = static_cast<nfds_t>( 1U );
        constexpr std::int64_t  minimumPollMilliseconds   = 1;
        constexpr std::int64_t  maximumPollMilliseconds =
            static_cast<std::int64_t>( std::numeric_limits<int>::max() );

        constexpr std::string_view xtestNameTag{ "XTEST" };
        constexpr std::string_view xAxisName{ "x" };
        constexpr std::string_view yAxisName{ "y" };
        constexpr std::string_view motionAxisName{ "motion" };
        constexpr std::string_view extensionUnavailableMessage{
            "XInput extension is unavailable"
        };
        constexpr std::string_view xi2QueryFailedMessage{
            "XInput2 version query failed"
        };
        constexpr std::string_view xi2UnavailableMessage{ "XInput2 2.1 is unavailable" };
        constexpr std::string_view deviceQueryFailedMessage{
            "XInput2 device query failed"
        };
        constexpr std::string_view requestFailureSeparator{ " failed with X error " };
        constexpr std::string_view selectEventsOperation{ "XISelectEvents raw input" };
        constexpr std::string_view selectionFlushFailedMessage{
            "XInput2 raw event selection flush failed"
        };
        constexpr std::string_view demandNotEnabledMessage{
            "event demand is not enabled"
        };
        constexpr std::string_view connectionFailedMessage{
            "XCB connection failed while waiting for events"
        };
        constexpr std::string_view fileDescriptorUnavailableMessage{
            "XCB connection file descriptor is unavailable"
        };
        constexpr std::string_view pollFailedMessage{ "poll on XCB connection failed" };

        template<typename T>
        using XcbOwned = std::unique_ptr<T, decltype( &std::free )>;

        template<typename T>
        [[nodiscard]]
        XcbOwned<T>
        take_xcb_owned( T* pointer ) noexcept
        {
            return XcbOwned<T>{ pointer, &std::free };
        }

        struct DecodedEventIdentity
        {
                bool                  decoded_any{};
                xcb_input_device_id_t sourceid{};
                xcb_input_device_id_t deviceid{};
                InjectionKind         kind{ InjectionKind::Motion };
                std::uint32_t         detail{ noEvents };
        };

        [[nodiscard]]
        grab::Result<std::uint8_t>
        require_xinput_extension( xcb_connection_t* connection )
        {
            const xcb_query_extension_reply_t* extension =
                xcb_get_extension_data( connection, &xcb_input_id );
            if( !extension || !extension->present )
            {
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                   std::string{ extensionUnavailableMessage } );
            }

            return extension->major_opcode;
        }

        [[nodiscard]]
        grab::Result<void>
        require_xi2( xcb_connection_t* connection )
        {
            xcb_generic_error_t* raw_error{};
            auto reply = take_xcb_owned( xcb_input_xi_query_version_reply(
                connection,
                xcb_input_xi_query_version( connection,
                                            requiredXiMajorVersion,
                                            requiredXiMinorVersion ),
                &raw_error
            ) );
            auto error = take_xcb_owned( raw_error );

            if( error || !reply )
            {
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                   std::string{ xi2QueryFailedMessage } );
            }

            if( reply->major_version <
                requiredXiMajorVersion ||
                ( reply->major_version ==
                  requiredXiMajorVersion &&
                  reply->minor_version < requiredXiMinorVersion ) )
            {
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                   std::string{ xi2UnavailableMessage } );
            }

            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        check_request( xcb_connection_t* connection,
                       xcb_void_cookie_t cookie,
                       std::string_view  operation )
        {
            auto error = take_xcb_owned( xcb_request_check( connection, cookie ) );
            if( !error )
            {
                return {};
            }

            std::string message{ operation };
            message.append( requestFailureSeparator );
            message.append( std::to_string( error->error_code ) );
            return grab::fail( grab::ErrorCode::ProtocolError, std::move( message ) );
        }

        [[nodiscard]]
        double
        fp3232_to_double( xcb_input_fp3232_t value ) noexcept
        {
            return static_cast<double>( value.integral ) +
                   static_cast<double>( value.frac ) /
                   fp3232FractionDenominator;
        }

        [[nodiscard]]
        grab::Event
        make_key_event( grab::EventKind                        kind,
                        const xcb_input_raw_key_press_event_t& raw )
        {
            return grab::Event{
                .timestamp = grab::kernel::now_timestamp_s(),
                .sequence  = initialSequence,
                .kind      = kind,
                .category  = grab::EventCategory::Input,
                .payload   = grab::Payload{ grab::InputKey{
                    .code = static_cast<std::uint32_t>( raw.detail ),
                    .name = std::string{}
                } }
            };
        }

        [[nodiscard]]
        grab::Event
        make_button_event( const xcb_input_raw_button_press_event_t& raw )
        {
            return grab::Event{
                .timestamp = grab::kernel::now_timestamp_s(),
                .sequence  = initialSequence,
                .kind      = grab::EventKind::MouseClick,
                .category  = grab::EventCategory::Input,
                .payload   = grab::Payload{ grab::MouseClick{
                    .button = static_cast<std::uint32_t>( raw.detail ),
                    .name   = std::string{}
                } }
            };
        }

        [[nodiscard]]
        grab::Event
        make_button_event( grab::EventKind                           kind,
                           const xcb_input_raw_button_press_event_t& raw )
        {
            return grab::Event{
                .timestamp = grab::kernel::now_timestamp_s(),
                .sequence  = initialSequence,
                .kind      = kind,
                .category  = grab::EventCategory::Input,
                .payload   = grab::Payload{ grab::MouseButton{
                    .button   = static_cast<std::uint32_t>( raw.detail ),
                    .name     = std::string{},
                    .position = std::nullopt,
                } }
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
                .sequence  = initialSequence,
                .kind      = grab::EventKind::MouseMove,
                .category  = grab::EventCategory::Input,
                .payload   = grab::Payload{
                    grab::MouseMove{ .axis = std::string{ axis }, .delta = delta }
                }
            };
        }

        void
        append_motion_axis( std::vector<grab::Event>& events,
                            double                    timestamp,
                            int                       axis,
                            double                    delta )
        {
            if( axis == xAxisValuator )
            {
                events.push_back( make_motion_event( timestamp, xAxisName, delta ) );
            }
            else if( axis == yAxisValuator )
            {
                events.push_back( make_motion_event( timestamp, yAxisName, delta ) );
            }
        }

        void
        append_motion_events( const xcb_input_raw_motion_event_t& raw,
                              std::vector<grab::Event>&           events )
        {
            const std::size_t initial_event_count = events.size();
            const int         mask_length =
                xcb_input_raw_button_press_valuator_mask_length( &raw );
            const int value_length =
                xcb_input_raw_button_press_axisvalues_raw_length( &raw );
            const std::uint32_t* valuator_mask =
                xcb_input_raw_button_press_valuator_mask( &raw );
            const xcb_input_fp3232_t* values =
                xcb_input_raw_button_press_axisvalues_raw( &raw );

            if( valuator_mask &&
                values &&
                mask_length >
                noValuatorEntries &&
                value_length > noValuatorEntries )
            {
                std::size_t value_index{};
                for( int word_index = noValuatorEntries; word_index < mask_length;
                     ++word_index )
                {
                    const std::uint32_t mask_word =
                        valuator_mask[static_cast<std::size_t>( word_index )];
                    for( int bit_index = noValuatorEntries; bit_index < bitsPerMaskWord;
                         ++bit_index )
                    {
                        const std::uint32_t bit =
                            firstMaskBit << static_cast<std::uint32_t>( bit_index );
                        if( ( mask_word & bit ) == noEvents )
                        {
                            continue;
                        }
                        if( value_index >= static_cast<std::size_t>( value_length ) )
                        {
                            break;
                        }

                        const int axis = word_index * bitsPerMaskWord + bit_index;
                        append_motion_axis( events,
                                            grab::kernel::now_timestamp_s(),
                                            axis,
                                            fp3232_to_double( values[value_index] ) );
                        ++value_index;
                    }
                }
            }

            if( events.size() == initial_event_count )
            {
                events.push_back( make_motion_event( grab::kernel::now_timestamp_s(),
                                                     motionAxisName,
                                                     static_cast<double>( noEvents ) ) );
            }
        }

        [[nodiscard]]
        DecodedEventIdentity
        append_decoded_event( const xcb_generic_event_t& raw_event,
                              std::uint8_t               extension_opcode,
                              std::vector<grab::Event>&  events )
        {
            DecodedEventIdentity identity{};
            if( static_cast<std::uint8_t>( raw_event.response_type &
                                           responseTypeMask ) !=
                static_cast<std::uint8_t>( XCB_GE_GENERIC ) )
            {
                return identity;
            }

            const void* event_storage = &raw_event;
            const auto* generic =
                static_cast<const xcb_ge_generic_event_t*>( event_storage );
            if( generic->extension != extension_opcode )
            {
                return identity;
            }

            const std::size_t initial_event_count = events.size();
            switch( generic->event_type )
            {
                case XCB_INPUT_RAW_KEY_PRESS :
                    {
                        const auto* raw =
                            static_cast<const xcb_input_raw_key_press_event_t*>(
                                event_storage
                            );
                        events.push_back( make_key_event( grab::EventKind::KeyDown,
                                                          *raw ) );
                        identity.sourceid = raw->sourceid;
                        identity.deviceid = raw->deviceid;
                        identity.kind     = InjectionKind::KeyPress;
                        identity.detail   = static_cast<std::uint32_t>( raw->detail );
                        break;
                    }
                case XCB_INPUT_RAW_KEY_RELEASE :
                    {
                        const auto* raw =
                            static_cast<const xcb_input_raw_key_release_event_t*>(
                                event_storage
                            );
                        events.push_back( make_key_event( grab::EventKind::KeyUp,
                                                          *raw ) );
                        identity.sourceid = raw->sourceid;
                        identity.deviceid = raw->deviceid;
                        identity.kind     = InjectionKind::KeyRelease;
                        identity.detail   = static_cast<std::uint32_t>( raw->detail );
                        break;
                    }
                case XCB_INPUT_RAW_BUTTON_PRESS :
                    {
                        const auto* raw =
                            static_cast<const xcb_input_raw_button_press_event_t*>(
                                event_storage
                            );
                        events.push_back( make_button_event( *raw ) );
                        events.push_back(
                            make_button_event( grab::EventKind::MouseButtonDown, *raw )
                        );
                        identity.sourceid = raw->sourceid;
                        identity.deviceid = raw->deviceid;
                        identity.kind     = InjectionKind::ButtonPress;
                        identity.detail   = static_cast<std::uint32_t>( raw->detail );
                        break;
                    }
                case XCB_INPUT_RAW_BUTTON_RELEASE :
                    {
                        const auto* raw =
                            static_cast<const xcb_input_raw_button_release_event_t*>(
                                event_storage
                            );
                        events.push_back(
                            make_button_event( grab::EventKind::MouseButtonUp, *raw )
                        );
                        identity.sourceid = raw->sourceid;
                        identity.deviceid = raw->deviceid;
                        identity.kind     = InjectionKind::ButtonRelease;
                        identity.detail   = static_cast<std::uint32_t>( raw->detail );
                        break;
                    }
                case XCB_INPUT_RAW_MOTION :
                    {
                        const auto* raw =
                            static_cast<const xcb_input_raw_motion_event_t*>(
                                event_storage
                            );
                        append_motion_events( *raw, events );
                        identity.sourceid = raw->sourceid;
                        identity.deviceid = raw->deviceid;
                        identity.kind     = InjectionKind::Motion;
                        identity.detail   = noEvents;
                        break;
                    }
                default :
                    break;
            }

            identity.decoded_any = events.size() > initial_event_count;
            return identity;
        }

        [[nodiscard]]
        std::optional<std::size_t>
        demand_slot( grab::EventKind kind ) noexcept
        {
            switch( kind )
            {
                case grab::EventKind::KeyDown :
                    return keyDownDemandSlot;
                case grab::EventKind::KeyUp :
                    return keyUpDemandSlot;
                case grab::EventKind::MouseClick :
                    return mouseClickDemandSlot;
                case grab::EventKind::MouseMove :
                    return mouseMoveDemandSlot;
                case grab::EventKind::MouseButtonDown :
                    return mouseButtonDownDemandSlot;
                case grab::EventKind::MouseButtonUp :
                    return mouseButtonUpDemandSlot;
                default :
                    return std::nullopt;
            }
        }

        [[nodiscard]]
        grab::Result<std::vector<std::uint16_t>>
        find_xtest_device_ids( xcb_connection_t* connection )
        {
            xcb_generic_error_t* raw_error{};
            auto                 reply = take_xcb_owned( xcb_input_xi_query_device_reply(
                connection,
                xcb_input_xi_query_device(
                    connection,
                    static_cast<xcb_input_device_id_t>( XCB_INPUT_DEVICE_ALL )
                ),
                &raw_error
            ) );
            auto                 error = take_xcb_owned( raw_error );
            if( error || !reply )
            {
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                   std::string{ deviceQueryFailedMessage } );
            }

            std::vector<std::uint16_t> device_ids;
            auto iterator = xcb_input_xi_query_device_infos_iterator( reply.get() );
            while( iterator.rem > noDeviceInfos )
            {
                const int name_length =
                    xcb_input_xi_device_info_name_length( iterator.data );
                const char* name = xcb_input_xi_device_info_name( iterator.data );
                if( name &&
                    name_length >
                    noNameBytes &&
                    std::string_view{ name, static_cast<std::size_t>( name_length ) }
                        .contains( xtestNameTag ) )
                {
                    device_ids.push_back( iterator.data->deviceid );
                }
                xcb_input_xi_device_info_next( &iterator );
            }

            return device_ids;
        }

    }

    X11EventSource::X11EventSource( xcb_connection_t*          connection,
                                    xcb_window_t               root,
                                    std::uint8_t               extension_opcode,
                                    std::vector<std::uint16_t> xtest_device_ids,
                                    InjectionLedger&           ledger ) noexcept :
        connection_{ connection },
        root_{ root },
        extension_opcode_{ extension_opcode },
        xtest_device_ids_{ std::move( xtest_device_ids ) },
        ledger_{ &ledger }
    {
    }

    grab::Result<std::unique_ptr<X11EventSource>>
    X11EventSource::open( xcb_connection_t* connection,
                          xcb_window_t      root,
                          InjectionLedger&  ledger )
    {
        auto extension_opcode = require_xinput_extension( connection );
        if( !extension_opcode )
        {
            return std::unexpected( std::move( extension_opcode.error() ) );
        }

        auto xi2 = require_xi2( connection );
        if( !xi2 )
        {
            return std::unexpected( std::move( xi2.error() ) );
        }

        auto xtest_device_ids = find_xtest_device_ids( connection );
        if( !xtest_device_ids )
        {
            return std::unexpected( std::move( xtest_device_ids.error() ) );
        }

        return std::unique_ptr<X11EventSource>{
            new X11EventSource{
                               connection, root,
                               *extension_opcode,
                               std::move( *xtest_device_ids ),
                               ledger
            }
        };
    }

    void
    X11EventSource::set_sink( EventSink sink )
    {
        const std::scoped_lock lock{ sink_mutex_ };
        sink_ = std::move( sink );
    }

    void
    X11EventSource::set_global_space( grab::CoordinateSpaceId space )
    {
        const std::scoped_lock lock{ state_mutex_ };
        global_space_ = space;
    }

    void
    X11EventSource::stamp_motion_batch_position( InjectionKind             kind,
                                                 std::vector<grab::Event>& events,
                                                 std::size_t               first_event )
    {
        if( kind !=
            InjectionKind::Motion &&
            kind !=
            InjectionKind::ButtonPress &&
            kind != InjectionKind::ButtonRelease )
        {
            return;
        }

        std::optional<grab::CoordinateSpaceId> space;
        {
            const std::scoped_lock lock{ state_mutex_ };
            space = global_space_;
        }
        if( !space.has_value() )
        {
            return;
        }

        xcb_generic_error_t* raw_error{};
        auto                 reply = take_xcb_owned(
            xcb_query_pointer_reply( connection_,
                                     xcb_query_pointer( connection_, root_ ),
                                     &raw_error )
        );
        auto error = take_xcb_owned( raw_error );
        if( error !=
            nullptr ||
            reply ==
            nullptr ||
            reply->same_screen == differentScreen )
        {
            return;
        }

        const grab::SpacePoint position{
            .x     = static_cast<double>( reply->root_x ),
            .y     = static_cast<double>( reply->root_y ),
            .space = *space,
        };
        for( std::size_t event_index = first_event; event_index < events.size();
             ++event_index )
        {
            auto* const motion =
                std::get_if<grab::MouseMove>( &events.at( event_index ).payload );
            if( motion != nullptr )
            {
                motion->position = position;
            }

            auto* const button =
                std::get_if<grab::MouseButton>( &events.at( event_index ).payload );
            if( button != nullptr )
            {
                button->position = position;
            }
        }
    }

    grab::Result<void>
    X11EventSource::enable( const grab::spi::EventSpec& spec )
    {
        const auto kind = grab::wire_kind( spec.name );
        const auto slot = kind ? demand_slot( *kind ) : std::nullopt;
        if( !slot )
        {
            // The wait engine enables arbitrary predicate names; only input kinds map to
            // XI2 raw masks.
            return {};
        }

        const std::scoped_lock lock{ state_mutex_ };
        const std::uint32_t    old_mask = active_mask();
        ++demand_refcounts_[*slot];
        const std::uint32_t new_mask = active_mask();
        if( old_mask == new_mask )
        {
            return {};
        }

        auto selected = select_events( new_mask );
        if( !selected )
        {
            --demand_refcounts_[*slot];
            return std::unexpected( std::move( selected.error() ) );
        }

        return {};
    }

    grab::Result<void>
    X11EventSource::disable( const grab::spi::EventSpec& spec )
    {
        const auto kind = grab::wire_kind( spec.name );
        const auto slot = kind ? demand_slot( *kind ) : std::nullopt;
        if( !slot )
        {
            // The wait engine enables arbitrary predicate names; only input kinds map to
            // XI2 raw masks.
            return {};
        }

        const std::scoped_lock lock{ state_mutex_ };
        if( demand_refcounts_[*slot] == noDemand )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               std::string{ demandNotEnabledMessage } );
        }

        const std::uint32_t old_mask = active_mask();
        --demand_refcounts_[*slot];
        const std::uint32_t new_mask = active_mask();
        if( old_mask == new_mask )
        {
            return {};
        }

        auto selected = select_events( new_mask );
        if( !selected )
        {
            ++demand_refcounts_[*slot];
            return std::unexpected( std::move( selected.error() ) );
        }

        return {};
    }

    std::uint32_t
    X11EventSource::active_mask() const noexcept
    {
        std::uint32_t mask = noEvents;
        if( demand_refcounts_[keyDownDemandSlot] > noDemand )
        {
            mask |= static_cast<std::uint32_t>( XCB_INPUT_XI_EVENT_MASK_RAW_KEY_PRESS );
        }
        if( demand_refcounts_[keyUpDemandSlot] > noDemand )
        {
            mask |=
                static_cast<std::uint32_t>( XCB_INPUT_XI_EVENT_MASK_RAW_KEY_RELEASE );
        }
        if( demand_refcounts_[mouseClickDemandSlot] >
            noDemand ||
            demand_refcounts_[mouseButtonDownDemandSlot] > noDemand )
        {
            mask |=
                static_cast<std::uint32_t>( XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_PRESS );
        }
        if( demand_refcounts_[mouseButtonUpDemandSlot] > noDemand )
        {
            mask |=
                static_cast<std::uint32_t>( XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_RELEASE );
        }
        if( demand_refcounts_[mouseMoveDemandSlot] > noDemand )
        {
            mask |= static_cast<std::uint32_t>( XCB_INPUT_XI_EVENT_MASK_RAW_MOTION );
        }
        return mask;
    }

    grab::Result<void>
    X11EventSource::select_events( std::uint32_t mask )
    {
        struct RawEventSelection
        {
                xcb_input_event_mask_t                  event_mask;
                std::array<std::uint32_t, rawMaskWords> mask_words;
        };

        RawEventSelection selection{
            .event_mask =
                {
                             .deviceid = static_cast<xcb_input_device_id_t>(
                             XCB_INPUT_DEVICE_ALL_MASTER
                             ), .mask_len = rawMaskWords,
                             },
            .mask_words = {mask                         },
        };
        auto selected =
            check_request( connection_,
                           xcb_input_xi_select_events_checked( connection_,
                                                               root_,
                                                               rawMaskCount,
                                                               &selection.event_mask ),
                           selectEventsOperation );
        if( !selected )
        {
            return std::unexpected( std::move( selected.error() ) );
        }

        if( xcb_flush( connection_ ) <=
            flushFailed ||
            xcb_connection_has_error( connection_ ) != xcbOk )
        {
            return grab::fail( grab::ErrorCode::DeviceInaccessible,
                               std::string{ selectionFlushFailedMessage } );
        }

        return {};
    }

    grab::Result<void>
    X11EventSource::wait_for_event( const grab::spi::EventSpec&   spec,
                                    const grab::OperationContext& context,
                                    std::chrono::nanoseconds      maximum_wait )
    {
        auto checked = context.check();
        if( !checked )
        {
            return std::unexpected( std::move( checked.error() ) );
        }

        const auto wake_at = std::chrono::steady_clock::now() +
                             std::min( maximum_wait, context.deadline.remaining() );
        auto       relevant_kind = grab::wire_kind( spec.name );
        if( relevant_kind && !demand_slot( *relevant_kind ) )
        {
            relevant_kind.reset();
        }

        for( ;; )
        {
            std::vector<grab::Event> pending;
            bool                     relevant{};
            while( auto raw_event = take_xcb_owned( xcb_poll_for_event( connection_ ) ) )
            {
                const std::size_t          first_pending_event = pending.size();
                const DecodedEventIdentity decoded =
                    append_decoded_event( *raw_event, extension_opcode_, pending );
                if( !decoded.decoded_any )
                {
                    continue;
                }

                stamp_motion_batch_position( decoded.kind,
                                             pending,
                                             first_pending_event );

                const bool source_is_xtest =
                    std::find( xtest_device_ids_.begin(),
                               xtest_device_ids_.end(),
                               decoded.sourceid ) != xtest_device_ids_.end();
                const bool device_is_xtest =
                    std::find( xtest_device_ids_.begin(),
                               xtest_device_ids_.end(),
                               decoded.deviceid ) != xtest_device_ids_.end();

                grab::EventOrigin origin = grab::EventOrigin::Physical;
                if( source_is_xtest || device_is_xtest )
                {
                    origin = ledger_->consume_match( decoded.kind, decoded.detail )
                               ? grab::EventOrigin::InjectedSelf
                               : grab::EventOrigin::InjectedOther;
                }
                // Physical devices cannot be exercised under Xvfb; default to physical
                // otherwise.

                for( std::size_t event_index = first_pending_event;
                     event_index < pending.size();
                     ++event_index )
                {
                    pending[event_index].origin = origin;
                    relevant = relevant ||
                               !relevant_kind ||
                               pending[event_index].kind == *relevant_kind;
                }
            }

            if( !pending.empty() )
            {
                EventSink sink_copy;
                {
                    const std::scoped_lock lock{ sink_mutex_ };
                    sink_copy = sink_;
                }
                if( sink_copy )
                {
                    for( auto& event : pending )
                    {
                        sink_copy( std::move( event ) );
                    }
                }
            }

            if( relevant )
            {
                return {};
            }
            if( xcb_connection_has_error( connection_ ) != xcbOk )
            {
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                   std::string{ connectionFailedMessage } );
            }

            checked = context.check();
            if( !checked )
            {
                return std::unexpected( std::move( checked.error() ) );
            }

            const auto now = std::chrono::steady_clock::now();
            if( now >= wake_at )
            {
                // The wait engine re-checks its predicate after this budget-only wake.
                return {};
            }

            const int file_descriptor = xcb_get_file_descriptor( connection_ );
            if( file_descriptor == invalidFd )
            {
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                   std::string{ fileDescriptorUnavailableMessage } );
            }

            pollfd poll_target{
                .fd      = file_descriptor,
                .events  = static_cast<short>( POLLIN ),
                .revents = noPollEvents,
            };
            const auto         remaining              = wake_at - now;
            const std::int64_t remaining_milliseconds = static_cast<std::int64_t>(
                std::chrono::ceil<std::chrono::milliseconds>( remaining ).count()
            );
            const int timeout_milliseconds =
                static_cast<int>( std::clamp( remaining_milliseconds,
                                              minimumPollMilliseconds,
                                              maximumPollMilliseconds ) );
            const int ready =
                poll( &poll_target, pollTargetCount, timeout_milliseconds );
            if( ready < xcbOk && errno == EINTR )
            {
                continue;
            }
            if( ready < xcbOk )
            {
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                   std::string{ pollFailedMessage } );
            }
        }
    }

}
