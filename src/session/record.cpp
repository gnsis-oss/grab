#include "grab/pid.hpp"
#include "grab/result.hpp"
#include "grab/workspace.hpp"
#include "session/record.hpp"

#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>    // IWYU pragma: keep
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace grab::session
{
    namespace
    {

        constexpr std::string_view nameField             = "name";
        constexpr std::string_view providerField         = "provider";
        constexpr std::string_view endpointField         = "endpoint";
        constexpr std::string_view controlSocketField    = "control_socket";
        constexpr std::string_view modeField             = "mode";
        constexpr std::string_view stateField            = "state";
        constexpr std::string_view widthField            = "width";
        constexpr std::string_view heightField           = "height";
        constexpr std::string_view supervisorPidField    = "supervisor_pid";
        constexpr std::string_view createdMonotonicField = "created_monotonic";

        using Json                                       = nlohmann::json;

        [[nodiscard]]
        const Json*
        find_field( const Json&      object,
                    std::string_view field )
        {
            const auto member = object.find( std::string{ field } );
            if( member == object.end() )
            {
                return nullptr;
            }
            return &*member;
        }

        [[nodiscard]]
        std::optional<std::string>
        string_field( const Json&      object,
                      std::string_view field )
        {
            const Json* value = find_field( object, field );
            if( value == nullptr || !value->is_string() )
            {
                return std::nullopt;
            }
            return value->get<std::string>();
        }

        [[nodiscard]]
        std::optional<std::uint64_t>
        uint64_field( const Json&      object,
                      std::string_view field )
        {
            const Json* value = find_field( object, field );
            if( value == nullptr || !value->is_number_integer() )
            {
                return std::nullopt;
            }
            if( value->is_number_unsigned() )
            {
                return value->get<std::uint64_t>();
            }

            const auto signed_value = value->get<std::int64_t>();
            if( signed_value < 0 )
            {
                return std::nullopt;
            }
            return static_cast<std::uint64_t>( signed_value );
        }

        [[nodiscard]]
        std::optional<std::int64_t>
        int64_field( const Json&      object,
                     std::string_view field )
        {
            const Json* value = find_field( object, field );
            if( value == nullptr || !value->is_number_integer() )
            {
                return std::nullopt;
            }
            if( !value->is_number_unsigned() )
            {
                return value->get<std::int64_t>();
            }

            const auto     unsigned_value = value->get<std::uint64_t>();
            constexpr auto maxSigned =
                static_cast<std::uint64_t>( std::numeric_limits<std::int64_t>::max() );
            if( unsigned_value > maxSigned )
            {
                return std::nullopt;
            }
            return static_cast<std::int64_t>( unsigned_value );
        }

        [[nodiscard]]
        bool
        fits_uint16( std::uint64_t value ) noexcept
        {
            return value <= std::numeric_limits<std::uint16_t>::max();
        }

        [[nodiscard]]
        grab::Result<SessionRecord>
        fail_record_field( std::string_view field )
        {
            std::string message{ "missing or malformed session record field: " };
            message += field;
            return grab::fail( ErrorCode::ProtocolError, std::move( message ) );
        }

    }    // namespace

    std::string
    to_json( const SessionRecord& record )
    {
        nlohmann::ordered_json object = nlohmann::ordered_json::object();
        object.emplace( std::string{ nameField }, record.name );
        object.emplace( std::string{ providerField }, record.provider );
        object.emplace( std::string{ endpointField }, record.endpoint );
        object.emplace( std::string{ controlSocketField }, record.control_socket );
        object.emplace( std::string{ modeField },
                        std::string{ mode_name( record.mode ) } );
        object.emplace( std::string{ stateField },
                        std::string{ state_name( record.state ) } );
        object.emplace( std::string{ widthField },
                        static_cast<std::uint64_t>( record.geometry.width ) );
        object.emplace( std::string{ heightField },
                        static_cast<std::uint64_t>( record.geometry.height ) );
        object.emplace( std::string{ supervisorPidField },
                        record.supervisor_pid.value() );
        object.emplace( std::string{ createdMonotonicField }, record.created_monotonic );
        return object.dump();
    }

    grab::Result<SessionRecord>
    parse_record( std::string_view text )
    {
        const Json object = Json::parse( text, nullptr, false );
        if( object.is_discarded() || !object.is_object() )
        {
            return fail_record_field( nameField );
        }

        auto name = string_field( object, nameField );
        if( !name.has_value() )
        {
            return fail_record_field( nameField );
        }

        auto provider = string_field( object, providerField );
        if( !provider.has_value() )
        {
            return fail_record_field( providerField );
        }

        auto endpoint = string_field( object, endpointField );
        if( !endpoint.has_value() )
        {
            return fail_record_field( endpointField );
        }

        auto control_socket = string_field( object, controlSocketField );
        if( !control_socket.has_value() )
        {
            return fail_record_field( controlSocketField );
        }

        const auto mode_text = string_field( object, modeField );
        if( !mode_text.has_value() )
        {
            return fail_record_field( modeField );
        }
        const auto mode = mode_from_string( *mode_text );
        if( !mode.has_value() )
        {
            return fail_record_field( modeField );
        }

        const auto state_text = string_field( object, stateField );
        if( !state_text.has_value() )
        {
            return fail_record_field( stateField );
        }
        const auto state = session_state_from_string( *state_text );
        if( !state.has_value() )
        {
            return fail_record_field( stateField );
        }

        const auto width = uint64_field( object, widthField );
        if( !width.has_value() || !fits_uint16( *width ) )
        {
            return fail_record_field( widthField );
        }

        const auto height = uint64_field( object, heightField );
        if( !height.has_value() || !fits_uint16( *height ) )
        {
            return fail_record_field( heightField );
        }

        const auto supervisor_pid = int64_field( object, supervisorPidField );
        if( !supervisor_pid.has_value() )
        {
            return fail_record_field( supervisorPidField );
        }

        const auto created_monotonic = uint64_field( object, createdMonotonicField );
        if( !created_monotonic.has_value() )
        {
            return fail_record_field( createdMonotonicField );
        }

        const WorkspaceGeometry geometry{
            .width  = static_cast<std::uint16_t>( *width ),
            .height = static_cast<std::uint16_t>( *height ),
        };

        return SessionRecord{
            .name              = std::move( *name ),
            .provider          = std::move( *provider ),
            .endpoint          = std::move( *endpoint ),
            .control_socket    = std::move( *control_socket ),
            .mode              = *mode,
            .geometry          = geometry,
            .state             = *state,
            .supervisor_pid    = grab::Pid{ *supervisor_pid },
            .created_monotonic = *created_monotonic,
        };
    }

}    // namespace grab::session
