#include "eventgrab/v1/events.pb.h"
#include "grab/event.hpp"
#include "grab/payload_fields.hpp"
#include "grab/pid.hpp"
#include "grab/result.hpp"
#include "transport/codec.hpp"
#include "transport/proto_descriptor.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace grab::transport
{
    namespace
    {

        constexpr std::uint64_t    noSequence     = 0U;
        constexpr std::string_view protocolPrefix = "malformed event: ";
        using grab::PayloadField;

        [[nodiscard]]
        std::unexpected<grab::Error>
        protocol_error( std::string message )
        {
            return grab::fail( grab::ErrorCode::ProtocolError,
                               std::string{ protocolPrefix } + std::move( message ) );
        }

        [[nodiscard]]
        std::string
        uint_to_string( std::uint32_t value )
        {
            return std::to_string( value );
        }

        [[nodiscard]]
        std::string
        double_to_string( double value )
        {
            std::ostringstream stream;
            stream << std::setprecision( std::numeric_limits<double>::max_digits10 )
                   << value;
            return stream.str();
        }

        [[nodiscard]]
        bool
        parse_uint32( std::string_view text,
                      std::uint32_t&   value ) noexcept
        {
            if( text.empty() )
            {
                return false;
            }

            std::uint32_t parsed     = 0U;
            const auto*   begin      = text.data();
            const auto    distance   = static_cast<std::ptrdiff_t>( text.size() );
            const auto*   end        = std::next( begin, distance );
            const auto    result     = std::from_chars( begin, end, parsed );
            const bool    all_parsed = result.ptr == end;
            if( result.ec != std::errc{} || !all_parsed )
            {
                return false;
            }

            value = parsed;
            return true;
        }

        [[nodiscard]]
        bool
        parse_double( std::string_view text,
                      double&          value ) noexcept
        {
            if( text.empty() )
            {
                return false;
            }

            double      parsed     = 0.0;
            const auto* begin      = text.data();
            const auto  distance   = static_cast<std::ptrdiff_t>( text.size() );
            const auto* end        = std::next( begin, distance );
            const auto  result     = std::from_chars( begin, end, parsed );
            const bool  all_parsed = result.ptr == end;
            if( result.ec != std::errc{} || !all_parsed )
            {
                return false;
            }

            value = parsed;
            return true;
        }

        [[nodiscard]]
        grab::Result<std::string>
        required_string( const eventgrab::v1::Event& wire,
                         std::string_view            key )
        {
            const auto& data = wire.data();
            const auto  iter = data.find( std::string{ key } );
            if( iter == data.end() )
            {
                return protocol_error( "missing data key " + std::string{ key } );
            }
            return iter->second;
        }

        [[nodiscard]]
        std::string
        optional_string( const eventgrab::v1::Event& wire,
                         std::string_view            key )
        {
            const auto& data = wire.data();
            const auto  iter = data.find( std::string{ key } );
            if( iter == data.end() )
            {
                return {};
            }
            return iter->second;
        }

        [[nodiscard]]
        grab::Result<std::uint32_t>
        required_uint32( const eventgrab::v1::Event& wire,
                         std::string_view            key )
        {
            auto text = required_string( wire, key );
            if( !text.has_value() )
            {
                return std::unexpected( text.error() );
            }

            std::uint32_t value = 0U;
            if( !parse_uint32( *text, value ) )
            {
                return protocol_error( "unparseable unsigned integer key " +
                                       std::string{ key } );
            }
            return value;
        }

        [[nodiscard]]
        grab::Result<double>
        required_double( const eventgrab::v1::Event& wire,
                         std::string_view            key )
        {
            auto text = required_string( wire, key );
            if( !text.has_value() )
            {
                return std::unexpected( text.error() );
            }

            double value = 0.0;
            if( !parse_double( *text, value ) )
            {
                return protocol_error( "unparseable floating point key " +
                                       std::string{ key } );
            }
            return value;
        }

        [[nodiscard]]
        grab::Result<double>
        optional_double( const eventgrab::v1::Event& wire,
                         std::string_view            key )
        {
            const auto& data = wire.data();
            const auto  iter = data.find( std::string{ key } );
            if( iter == data.end() )
            {
                return 0.0;
            }

            double value = 0.0;
            if( !parse_double( iter->second, value ) )
            {
                return protocol_error( "unparseable floating point key " +
                                       std::string{ key } );
            }
            return value;
        }

        void
        set_data( eventgrab::v1::Event& wire,
                  std::string_view      key,
                  std::string           value )
        {
            auto* const data = wire.mutable_data();
            auto [iter, inserted] =
                data->try_emplace( std::string{ key }, std::move( value ) );
            if( !inserted )
            {
                iter->second = std::move( value );
            }
        }

        void
        encode_input_key( eventgrab::v1::Event& wire,
                          const grab::InputKey& payload )
        {
            set_data( wire,
                      grab::field_name( PayloadField::KeyCode ),
                      uint_to_string( payload.code ) );
            set_data( wire, grab::field_name( PayloadField::KeyName ), payload.name );
        }

        void
        encode_key_combo( eventgrab::v1::Event& wire,
                          const grab::KeyCombo& payload )
        {
            set_data( wire, grab::field_name( PayloadField::Text ), payload.text );
        }

        void
        encode_mouse_click( eventgrab::v1::Event&   wire,
                            const grab::MouseClick& payload )
        {
            set_data( wire,
                      grab::field_name( PayloadField::Button ),
                      uint_to_string( payload.button ) );
            set_data( wire, grab::field_name( PayloadField::ButtonName ), payload.name );
        }

        void
        encode_mouse_move( eventgrab::v1::Event&  wire,
                           const grab::MouseMove& payload )
        {
            set_data( wire, grab::field_name( PayloadField::Axis ), payload.axis );
            set_data( wire,
                      grab::field_name( PayloadField::Delta ),
                      double_to_string( payload.delta ) );
        }

        void
        encode_idle( eventgrab::v1::Event& wire,
                     const grab::Idle&     payload )
        {
            set_data( wire,
                      grab::field_name( PayloadField::IdleSeconds ),
                      double_to_string( payload.idle_s ) );
        }

        void
        encode_window_change( eventgrab::v1::Event&     wire,
                              const grab::WindowChange& payload )
        {
            set_data( wire, grab::field_name( PayloadField::App ), payload.app );
            set_data( wire,
                      grab::field_name( PayloadField::Pid ),
                      payload.pid.to_string() );
            set_data( wire, grab::field_name( PayloadField::Title ), payload.title );
            set_data( wire,
                      grab::field_name( PayloadField::PrevTitle ),
                      payload.prev_title );
            set_data( wire,
                      grab::field_name( PayloadField::DurationSeconds ),
                      double_to_string( payload.duration_s ) );
        }

        void
        encode_a11y_event( eventgrab::v1::Event&  wire,
                           grab::EventKind        kind,
                           const grab::A11yEvent& payload )
        {
            set_data( wire, grab::field_name( PayloadField::App ), payload.app );
            set_data( wire, grab::field_name( PayloadField::Role ), payload.role );
            set_data( wire, grab::field_name( PayloadField::Name ), payload.name );
            if( kind == grab::EventKind::A11yStateChanged )
            {
                set_data( wire,
                          grab::field_name( PayloadField::State ),
                          payload.detail );
                return;
            }
            set_data( wire, grab::field_name( PayloadField::Detail ), payload.detail );
        }

        void
        encode_integration_event( eventgrab::v1::Event&         wire,
                                  const grab::IntegrationEvent& payload )
        {
            set_data( wire, grab::field_name( PayloadField::App ), payload.app );
            set_data( wire, grab::field_name( PayloadField::Title ), payload.title );
            set_data( wire, grab::field_name( PayloadField::Detail ), payload.detail );
            set_data( wire, grab::field_name( PayloadField::Json ), payload.json );
        }

        void
        encode_browser_tab( eventgrab::v1::Event&   wire,
                            const grab::BrowserTab& payload )
        {
            set_data( wire, grab::field_name( PayloadField::App ), payload.app );
            set_data( wire,
                      grab::field_name( PayloadField::Pid ),
                      payload.pid.to_string() );
            set_data( wire,
                      grab::field_name( PayloadField::TabTitle ),
                      payload.tab_title );
            set_data( wire,
                      grab::field_name( PayloadField::PrevTabTitle ),
                      payload.prev_tab_title );
        }

        void
        encode_state_snapshot( eventgrab::v1::Event&      wire,
                               const grab::StateSnapshot& payload )
        {
            set_data( wire, grab::field_name( PayloadField::Json ), payload.json );
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
        grab::Result<grab::InputKey>
        decode_input_key( const eventgrab::v1::Event& wire )
        {
            auto code =
                required_uint32( wire, grab::field_name( PayloadField::KeyCode ) );
            if( !code.has_value() )
            {
                return std::unexpected( code.error() );
            }

            return grab::InputKey{
                .code = *code,
                .name =
                    optional_string( wire, grab::field_name( PayloadField::KeyName ) ),
            };
        }

        [[nodiscard]]
        grab::Result<grab::KeyCombo>
        decode_key_combo( const eventgrab::v1::Event& wire )
        {
            auto text = required_string( wire, grab::field_name( PayloadField::Text ) );
            if( !text.has_value() )
            {
                return std::unexpected( text.error() );
            }

            return grab::KeyCombo{ .text = *text };
        }

        [[nodiscard]]
        grab::Result<grab::MouseClick>
        decode_mouse_click( const eventgrab::v1::Event& wire )
        {
            auto button =
                required_uint32( wire, grab::field_name( PayloadField::Button ) );
            if( !button.has_value() )
            {
                return std::unexpected( button.error() );
            }

            return grab::MouseClick{
                .button = *button,
                .name = optional_string( wire,
                                         grab::field_name( PayloadField::ButtonName ) ),
            };
        }

        [[nodiscard]]
        grab::Result<grab::MouseMove>
        decode_mouse_move( const eventgrab::v1::Event& wire )
        {
            auto axis = required_string( wire, grab::field_name( PayloadField::Axis ) );
            if( !axis.has_value() )
            {
                return std::unexpected( axis.error() );
            }

            auto delta =
                required_double( wire, grab::field_name( PayloadField::Delta ) );
            if( !delta.has_value() )
            {
                return std::unexpected( delta.error() );
            }

            return grab::MouseMove{ .axis = *axis, .delta = *delta };
        }

        [[nodiscard]]
        grab::Result<grab::Idle>
        decode_idle( const eventgrab::v1::Event& wire,
                     grab::EventKind             kind )
        {
            auto idle_s =
                kind == grab::EventKind::IdleStart
                    ? required_double( wire,
                                       grab::field_name( PayloadField::IdleSeconds ) )
                    : optional_double( wire,
                                       grab::field_name( PayloadField::IdleSeconds ) );
            if( !idle_s.has_value() )
            {
                return std::unexpected( idle_s.error() );
            }

            return grab::Idle{ .idle_s = *idle_s };
        }

        [[nodiscard]]
        grab::Result<grab::WindowChange>
        decode_window_change( const eventgrab::v1::Event& wire,
                              grab::EventKind             kind )
        {
            auto app = required_string( wire, grab::field_name( PayloadField::App ) );
            if( !app.has_value() )
            {
                return std::unexpected( app.error() );
            }
            auto pid = required_string( wire, grab::field_name( PayloadField::Pid ) );
            if( !pid.has_value() )
            {
                return std::unexpected( pid.error() );
            }
            auto title =
                required_string( wire, grab::field_name( PayloadField::Title ) );
            if( !title.has_value() )
            {
                return std::unexpected( title.error() );
            }
            const auto prev_title =
                optional_string( wire, grab::field_name( PayloadField::PrevTitle ) );
            auto duration_s = kind == grab::EventKind::WindowClosed
                                ? required_double(
                                      wire,
                                      grab::field_name( PayloadField::DurationSeconds )
                                  )
                                : optional_double(
                                      wire,
                                      grab::field_name( PayloadField::DurationSeconds )
                                  );
            if( !duration_s.has_value() )
            {
                return std::unexpected( duration_s.error() );
            }

            return grab::WindowChange{
                .app        = *app,
                .pid        = grab::Pid::from_string( *pid ),
                .title      = *title,
                .prev_title = prev_title,
                .duration_s = *duration_s,
            };
        }

        [[nodiscard]]
        grab::Result<grab::A11yEvent>
        decode_a11y_event( const eventgrab::v1::Event& wire,
                           grab::EventKind             kind )
        {
            auto name = required_string( wire, grab::field_name( PayloadField::Name ) );
            if( !name.has_value() )
            {
                return std::unexpected( name.error() );
            }

            if( kind == grab::EventKind::A11yFocusChanged )
            {
                auto app =
                    required_string( wire, grab::field_name( PayloadField::App ) );
                if( !app.has_value() )
                {
                    return std::unexpected( app.error() );
                }
                auto role =
                    required_string( wire, grab::field_name( PayloadField::Role ) );
                if( !role.has_value() )
                {
                    return std::unexpected( role.error() );
                }

                return grab::A11yEvent{
                    .app  = *app,
                    .role = *role,
                    .name = *name,
                    .detail =
                        optional_string( wire,
                                         grab::field_name( PayloadField::Detail ) ),
                };
            }

            if( kind == grab::EventKind::A11yStateChanged )
            {
                auto detail =
                    required_string( wire, grab::field_name( PayloadField::State ) );
                if( !detail.has_value() )
                {
                    return std::unexpected( detail.error() );
                }

                return grab::A11yEvent{
                    .app =
                        optional_string( wire, grab::field_name( PayloadField::App ) ),
                    .role =
                        optional_string( wire, grab::field_name( PayloadField::Role ) ),
                    .name   = *name,
                    .detail = *detail,
                };
            }

            return grab::A11yEvent{
                .app  = optional_string( wire, grab::field_name( PayloadField::App ) ),
                .role = optional_string( wire, grab::field_name( PayloadField::Role ) ),
                .name = *name,
                .detail =
                    optional_string( wire, grab::field_name( PayloadField::Detail ) ),
            };
        }

        [[nodiscard]]
        grab::Result<grab::IntegrationEvent>
        decode_integration_event( const eventgrab::v1::Event& wire )
        {
            auto app = required_string( wire, grab::field_name( PayloadField::App ) );
            if( !app.has_value() )
            {
                return std::unexpected( app.error() );
            }
            auto title =
                required_string( wire, grab::field_name( PayloadField::Title ) );
            if( !title.has_value() )
            {
                return std::unexpected( title.error() );
            }
            auto detail =
                required_string( wire, grab::field_name( PayloadField::Detail ) );
            if( !detail.has_value() )
            {
                return std::unexpected( detail.error() );
            }
            auto json = required_string( wire, grab::field_name( PayloadField::Json ) );
            if( !json.has_value() )
            {
                return std::unexpected( json.error() );
            }

            return grab::IntegrationEvent{
                .app    = *app,
                .title  = *title,
                .detail = *detail,
                .json   = *json,
            };
        }

        [[nodiscard]]
        grab::Result<grab::BrowserTab>
        decode_browser_tab( const eventgrab::v1::Event& wire )
        {
            auto app = required_string( wire, grab::field_name( PayloadField::App ) );
            if( !app.has_value() )
            {
                return std::unexpected( app.error() );
            }
            auto pid = required_string( wire, grab::field_name( PayloadField::Pid ) );
            if( !pid.has_value() )
            {
                return std::unexpected( pid.error() );
            }
            auto tab_title =
                required_string( wire, grab::field_name( PayloadField::TabTitle ) );
            if( !tab_title.has_value() )
            {
                return std::unexpected( tab_title.error() );
            }
            return grab::BrowserTab{
                .app       = *app,
                .pid       = grab::Pid::from_string( *pid ),
                .tab_title = *tab_title,
                .prev_tab_title =
                    optional_string( wire,
                                     grab::field_name( PayloadField::PrevTabTitle ) ),
            };
        }

        [[nodiscard]]
        grab::Result<grab::StateSnapshot>
        decode_state_snapshot( const eventgrab::v1::Event& wire )
        {
            auto json = required_string( wire, grab::field_name( PayloadField::Json ) );
            if( !json.has_value() )
            {
                return std::unexpected( json.error() );
            }

            return grab::StateSnapshot{ .json = *json };
        }

        [[nodiscard]]
        grab::Result<grab::Payload>
        decode_payload( const eventgrab::v1::Event& wire,
                        grab::EventKind             kind )
        {
            switch( kind )
            {
                case grab::EventKind::KeyDown :
                case grab::EventKind::KeyUp :
                    return decode_input_key( wire );
                case grab::EventKind::KeyCombo :
                    return decode_key_combo( wire );
                case grab::EventKind::MouseClick :
                    return decode_mouse_click( wire );
                case grab::EventKind::MouseMove :
                    return decode_mouse_move( wire );
                case grab::EventKind::IdleStart :
                case grab::EventKind::IdleEnd :
                    return decode_idle( wire, kind );
                case grab::EventKind::WindowFocusChanged :
                case grab::EventKind::WindowTitleChanged :
                case grab::EventKind::WindowCreated :
                case grab::EventKind::WindowClosed :
                    return decode_window_change( wire, kind );
                case grab::EventKind::A11yButtonClicked :
                case grab::EventKind::A11yMenuOpened :
                case grab::EventKind::A11yMenuClosed :
                case grab::EventKind::A11yFocusChanged :
                case grab::EventKind::A11yTextChanged :
                case grab::EventKind::A11yStateChanged :
                    return decode_a11y_event( wire, kind );
                case grab::EventKind::AppTabChanged :
                case grab::EventKind::AppContextUpdate :
                    return decode_integration_event( wire );
                case grab::EventKind::BrowserTabSwitched :
                    return decode_browser_tab( wire );
                case grab::EventKind::StateSnapshot :
                    return decode_state_snapshot( wire );
                case grab::EventKind::Unspecified :
                    return protocol_error( "unknown event kind" );
            }

            return protocol_error( "unknown event kind" );
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

        switch( event.kind )
        {
            case grab::EventKind::KeyDown :
            case grab::EventKind::KeyUp :
                encode_input_key( wire, std::get<grab::InputKey>( event.payload ) );
                break;
            case grab::EventKind::KeyCombo :
                encode_key_combo( wire, std::get<grab::KeyCombo>( event.payload ) );
                break;
            case grab::EventKind::MouseClick :
                encode_mouse_click( wire, std::get<grab::MouseClick>( event.payload ) );
                break;
            case grab::EventKind::MouseMove :
                encode_mouse_move( wire, std::get<grab::MouseMove>( event.payload ) );
                break;
            case grab::EventKind::IdleStart :
            case grab::EventKind::IdleEnd :
                encode_idle( wire, std::get<grab::Idle>( event.payload ) );
                break;
            case grab::EventKind::WindowFocusChanged :
            case grab::EventKind::WindowTitleChanged :
            case grab::EventKind::WindowCreated :
            case grab::EventKind::WindowClosed :
                encode_window_change( wire,
                                      std::get<grab::WindowChange>( event.payload ) );
                break;
            case grab::EventKind::A11yButtonClicked :
            case grab::EventKind::A11yMenuOpened :
            case grab::EventKind::A11yMenuClosed :
            case grab::EventKind::A11yFocusChanged :
            case grab::EventKind::A11yTextChanged :
            case grab::EventKind::A11yStateChanged :
                encode_a11y_event( wire,
                                   event.kind,
                                   std::get<grab::A11yEvent>( event.payload ) );
                break;
            case grab::EventKind::AppTabChanged :
            case grab::EventKind::AppContextUpdate :
                encode_integration_event(
                    wire,
                    std::get<grab::IntegrationEvent>( event.payload )
                );
                break;
            case grab::EventKind::BrowserTabSwitched :
                encode_browser_tab( wire, std::get<grab::BrowserTab>( event.payload ) );
                break;
            case grab::EventKind::StateSnapshot :
                encode_state_snapshot( wire,
                                       std::get<grab::StateSnapshot>( event.payload ) );
                break;
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

        return grab::Event{
            .timestamp = wire.timestamp(),
            .sequence  = noSequence,
            .kind      = *kind,
            .category  = *category,
            .payload   = std::move( *payload ),
        };
    }

}    // namespace grab::transport
