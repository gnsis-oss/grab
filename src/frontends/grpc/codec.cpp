#include "compat/eventgrab_v1/v1_payload_codec.hpp"
#include "eventgrab/v1/events.pb.h"
#include "frontends/grpc/codec.hpp"
#include "frontends/grpc/proto_descriptor.hpp"
#include "grab/event.hpp"
#include "grab/ids.hpp"
#include "grab/origin.hpp"
#include "grab/payload_fields.hpp"
#include "grab/result.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace grab::transport
{
    namespace
    {

        constexpr std::uint64_t    noSequence     = 0U;
        constexpr std::string_view protocolPrefix = "malformed event: ";
        using grab::PayloadField;

        constexpr std::size_t uuidBytes = 16U;

        [[nodiscard]]
        std::unexpected<grab::Error>
        protocol_error( std::string message );

        [[nodiscard]]
        eventgrab::v1::EventOrigin
        encode_origin( grab::EventOrigin origin ) noexcept
        {
            switch( origin )
            {
                case grab::EventOrigin::Physical :
                    return eventgrab::v1::EVENT_ORIGIN_PHYSICAL;
                case grab::EventOrigin::InjectedSelf :
                    return eventgrab::v1::EVENT_ORIGIN_INJECTED_SELF;
                case grab::EventOrigin::InjectedOther :
                    return eventgrab::v1::EVENT_ORIGIN_INJECTED_OTHER;
                case grab::EventOrigin::Unknown :
                    return eventgrab::v1::EVENT_ORIGIN_UNKNOWN;
            }

            return eventgrab::v1::EVENT_ORIGIN_UNKNOWN;
        }

        [[nodiscard]]
        grab::EventOrigin
        decode_origin( eventgrab::v1::EventOrigin origin ) noexcept
        {
            switch( origin )
            {
                case eventgrab::v1::EVENT_ORIGIN_PHYSICAL :
                    return grab::EventOrigin::Physical;
                case eventgrab::v1::EVENT_ORIGIN_INJECTED_SELF :
                    return grab::EventOrigin::InjectedSelf;
                case eventgrab::v1::EVENT_ORIGIN_INJECTED_OTHER :
                    return grab::EventOrigin::InjectedOther;
                default :
                    return grab::EventOrigin::Unknown;
            }
        }

        [[nodiscard]]
        std::string
        encode_operation_id( const grab::OperationId& operation )
        {
            std::string encoded;
            encoded.reserve( operation.value.bytes.size() );
            for( const auto byte : operation.value.bytes )
            {
                encoded.push_back( static_cast<char>( byte ) );
            }
            return encoded;
        }

        [[nodiscard]]
        grab::Result<grab::OperationId>
        decode_operation_id( std::string_view encoded )
        {
            if( encoded.size() != uuidBytes )
            {
                return protocol_error( "cause must contain a 16-byte UUID" );
            }

            std::array<std::uint8_t, uuidBytes> bytes{};
            std::ranges::transform( encoded,
                                    bytes.begin(),
                                    []( char byte )
                                    {
                                        return static_cast<std::uint8_t>(
                                            static_cast<unsigned char>( byte )
                                        );
                                    } );
            return grab::OperationId{ .value = grab::Uuid{ .bytes = bytes } };
        }

        [[nodiscard]]
        std::unexpected<grab::Error>
        protocol_error( std::string message )
        {
            return grab::fail( grab::ErrorCode::ProtocolError,
                               std::string{ protocolPrefix } + std::move( message ) );
        }

        void
        encode_state_snapshot( eventgrab::v1::Event&      wire,
                               const grab::StateSnapshot& payload )
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            ( *wire.mutable_data() )[std::string{
                grab::field_name( PayloadField::Json )
            }] = payload.json;
        }

        [[nodiscard]]
        grab::Result<void>
        validate_size( const eventgrab::v1::Event& wire )
        {
            if( wire.data_size() > maxDataEntries )
            {
                return protocol_error( "too many data entries" );
            }

            for( const auto& [key, value] : wire.data() )
            {
                if( key.size() >
                    static_cast<std::size_t>( maxValueBytes ) ||
                    value.size() > static_cast<std::size_t>( maxValueBytes ) )
                {
                    return protocol_error( "data entry exceeds maximum byte size" );
                }
            }

            return {};
        }

        [[nodiscard]]
        grab::Result<grab::StateSnapshot>
        decode_state_snapshot( const eventgrab::v1::Event& wire )
        {
            const auto  json_key = std::string{ grab::field_name( PayloadField::Json ) };
            const auto& data     = wire.data();
            const auto  iter     = data.find( json_key );
            if( iter == data.end() )
            {
                return protocol_error( "missing data key " + json_key );
            }

            return grab::StateSnapshot{ .json = iter->second };
        }

        [[nodiscard]]
        grab::Result<grab::Payload>
        decode_payload( const eventgrab::v1::Event& wire,
                        grab::EventKind             kind )
        {
            if( grab::compat::eventgrab_v1::is_v1_payload_kind( kind ) )
            {
                return grab::compat::eventgrab_v1::decode_v1_payload( wire, kind );
            }

            switch( kind )
            {
                case grab::EventKind::StateSnapshot :
                    return decode_state_snapshot( wire );
                case grab::EventKind::NodeAdded :
                case grab::EventKind::NodeRemoved :
                case grab::EventKind::NodeChanged :
                case grab::EventKind::RelationAdded :
                case grab::EventKind::RelationRemoved :
                case grab::EventKind::ActiveChildChanged :
                    if( !wire.has_graph_change() )
                    {
                        return protocol_error( "missing graph_change payload" );
                    }
                    return grab::GraphChange{
                        .node            = wire.graph_change().node(),
                        .related         = wire.graph_change().related(),
                        .relation        = wire.graph_change().relation(),
                        .previous_active = wire.graph_change().previous_active(),
                    };
                case grab::EventKind::Unspecified :
                default :
                    return protocol_error( "unknown event kind" );
            }
        }

    }    // namespace

    grab::Result<eventgrab::v1::Event>
    to_wire( const grab::Event& event )
    {
        if( event.kind == grab::EventKind::Unspecified )
        {
            return protocol_error( "unknown event kind" );
        }

        eventgrab::v1::Event wire;
        wire.set_kind( to_wire_kind( event.kind ) );
        wire.set_category( to_wire_category( event.category ) );
        wire.set_timestamp( event.timestamp );
        wire.set_origin( encode_origin( event.origin ) );
        if( event.subject.has_value() )
        {
            auto* const subject = wire.mutable_subject();
            subject->set_runtime( event.subject->runtime.value );
            subject->set_tree( event.subject->tree );
            subject->set_epoch( event.subject->epoch.value );
            subject->set_node( event.subject->node );
            subject->set_revision( event.subject->revision );
        }
        if( event.cause.has_value() )
        {
            wire.set_cause( encode_operation_id( *event.cause ) );
        }
        if( event.before_revision.has_value() )
        {
            wire.set_before_revision( *event.before_revision );
        }
        if( event.after_revision.has_value() )
        {
            wire.set_after_revision( *event.after_revision );
        }

        switch( event.kind )
        {
            case grab::EventKind::KeyDown :
            case grab::EventKind::KeyUp :
            case grab::EventKind::KeyCombo :
            case grab::EventKind::MouseClick :
            case grab::EventKind::MouseMove :
            case grab::EventKind::IdleStart :
            case grab::EventKind::IdleEnd :
            case grab::EventKind::WindowFocusChanged :
            case grab::EventKind::WindowTitleChanged :
            case grab::EventKind::WindowCreated :
            case grab::EventKind::WindowClosed :
            case grab::EventKind::A11yButtonClicked :
            case grab::EventKind::A11yMenuOpened :
            case grab::EventKind::A11yMenuClosed :
            case grab::EventKind::A11yFocusChanged :
            case grab::EventKind::A11yTextChanged :
            case grab::EventKind::A11yStateChanged :
            case grab::EventKind::AppTabChanged :
            case grab::EventKind::AppContextUpdate :
                grab::compat::eventgrab_v1::encode_v1_payload( wire,
                                                               event.kind,
                                                               event.payload );
                break;
            case grab::EventKind::StateSnapshot :
                encode_state_snapshot( wire,
                                       std::get<grab::StateSnapshot>( event.payload ) );
                break;
            case grab::EventKind::NodeAdded :
            case grab::EventKind::NodeRemoved :
            case grab::EventKind::NodeChanged :
            case grab::EventKind::RelationAdded :
            case grab::EventKind::RelationRemoved :
            case grab::EventKind::ActiveChildChanged :
                {
                    const auto& payload = std::get<grab::GraphChange>( event.payload );
                    auto* const graph_change = wire.mutable_graph_change();
                    graph_change->set_node( payload.node );
                    graph_change->set_related( payload.related );
                    graph_change->set_relation( payload.relation );
                    graph_change->set_previous_active( payload.previous_active );
                    break;
                }
            case grab::EventKind::Unspecified :
                return protocol_error( "unknown event kind" );
        }

        return wire;
    }

    grab::Result<grab::Event>
    from_wire( const eventgrab::v1::Event& wire )
    {
        auto size_status = validate_size( wire );
        if( !size_status.has_value() )
        {
            return std::unexpected( size_status.error() );
        }

        const auto kind = grab::transport::to_grab_kind( wire.kind() );
        if( !kind.has_value() || *kind == grab::EventKind::Unspecified )
        {
            return protocol_error( "unknown event kind" );
        }

        const auto category = grab::transport::to_grab_category( wire.category() );
        if( !category.has_value() )
        {
            return protocol_error( "unknown event category" );
        }

        auto payload = decode_payload( wire, *kind );
        if( !payload.has_value() )
        {
            return std::unexpected( payload.error() );
        }

        grab::Event event{
            .timestamp = wire.timestamp(),
            .sequence  = noSequence,
            .kind      = *kind,
            .category  = *category,
            .payload   = std::move( *payload ),
        };
        event.origin = decode_origin( wire.origin() );
        if( wire.has_subject() )
        {
            event.subject = grab::EventSubject{
                .runtime  = grab::RuntimeId{ wire.subject().runtime() },
                .tree     = wire.subject().tree(),
                .epoch    = grab::TreeEpoch{ wire.subject().epoch() },
                .node     = wire.subject().node(),
                .revision = wire.subject().revision(),
            };
        }
        if( wire.has_cause() )
        {
            auto cause = decode_operation_id( wire.cause() );
            if( !cause.has_value() )
            {
                return std::unexpected( cause.error() );
            }
            event.cause = *cause;
        }
        if( wire.has_before_revision() )
        {
            event.before_revision = wire.before_revision();
        }
        if( wire.has_after_revision() )
        {
            event.after_revision = wire.after_revision();
        }
        return event;
    }

}    // namespace grab::transport
