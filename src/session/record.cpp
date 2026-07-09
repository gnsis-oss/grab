#include "core/json.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "session/record.hpp"

#include <charconv>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace grab::session
{
    namespace
    {

        constexpr std::string_view name_field              = "name";
        constexpr std::string_view provider_field          = "provider";
        constexpr std::string_view endpoint_field          = "endpoint";
        constexpr std::string_view control_socket_field    = "control_socket";
        constexpr std::string_view mode_field              = "mode";
        constexpr std::string_view state_field             = "state";
        constexpr std::string_view width_field             = "width";
        constexpr std::string_view height_field            = "height";
        constexpr std::string_view supervisor_pid_field    = "supervisor_pid";
        constexpr std::string_view created_monotonic_field = "created_monotonic";
        constexpr std::string_view unicode_control_prefix  = "00";
        constexpr unsigned int     decimal_digit_count     = 10U;
        constexpr unsigned int     high_nibble_shift       = 4U;
        constexpr auto             quote_count             = 2U;
        constexpr auto             unicode_digit_count     = 2U;

        [[nodiscard]]
        bool
        is_whitespace( char value ) noexcept
        {
            switch( value )
            {
                case ' ' :
                case '\n' :
                case '\r' :
                case '\t' :
                    return true;
                default :
                    return false;
            }
        }

        [[nodiscard]]
        bool
        is_digit( char value ) noexcept
        {
            return value >= '0' && value <= '9';
        }

        [[nodiscard]]
        char
        char_at( std::string_view            text,
                 std::string_view::size_type position )
        {
            return text.at( position );
        }

        [[nodiscard]]
        std::optional<unsigned int>
        hex_value( char value ) noexcept
        {
            if( value >= '0' && value <= '9' )
            {
                return static_cast<unsigned int>( value - '0' );
            }
            if( value >= 'a' && value <= 'f' )
            {
                return decimal_digit_count + static_cast<unsigned int>( value - 'a' );
            }
            if( value >= 'A' && value <= 'F' )
            {
                return decimal_digit_count + static_cast<unsigned int>( value - 'A' );
            }
            return std::nullopt;
        }

        [[nodiscard]]
        std::string
        quoted_key( std::string_view key )
        {
            std::string token;
            token.reserve( key.size() + quote_count );
            token += '"';
            token += key;
            token += '"';
            return token;
        }

        [[nodiscard]]
        std::string_view::size_type
        skip_whitespace( std::string_view            text,
                         std::string_view::size_type position ) noexcept
        {
            while( position < text.size() && is_whitespace( char_at( text, position ) ) )
            {
                ++position;
            }
            return position;
        }

        [[nodiscard]]
        std::optional<std::string_view::size_type>
        find_value_start( std::string_view text,
                          std::string_view key )
        {
            const auto token   = quoted_key( key );
            const auto key_pos = text.find( token );
            if( key_pos == std::string_view::npos )
            {
                return std::nullopt;
            }

            auto position = key_pos + token.size();
            position      = skip_whitespace( text, position );
            if( position >= text.size() || char_at( text, position ) != ':' )
            {
                return std::nullopt;
            }

            ++position;
            return skip_whitespace( text, position );
        }

        [[nodiscard]]
        std::optional<char>
        read_unicode_control_escape( std::string_view             text,
                                     std::string_view::size_type& position )
        {
            const auto required_size =
                unicode_control_prefix.size() + unicode_digit_count;
            if( text.size() - position < required_size )
            {
                return std::nullopt;
            }
            if( text.substr( position, unicode_control_prefix.size() ) !=
                unicode_control_prefix )
            {
                return std::nullopt;
            }

            position        += unicode_control_prefix.size();
            const auto high  = hex_value( char_at( text, position ) );
            ++position;
            const auto low = hex_value( char_at( text, position ) );
            ++position;
            if( !high.has_value() || !low.has_value() )
            {
                return std::nullopt;
            }

            const auto byte = ( *high << high_nibble_shift ) | *low;
            return static_cast<char>( byte );
        }

        // Extract a quoted string value for "key" from a flat one-level object.
        // Returns std::nullopt if the key or its value is absent/malformed.
        [[nodiscard]]
        std::optional<std::string>
        find_string( std::string_view text,
                     std::string_view key )
        {
            auto position = find_value_start( text, key );
            if( !position.has_value() ||
                *position >=
                text.size() ||
                char_at( text, *position ) != '"' )
            {
                return std::nullopt;
            }

            ++*position;
            std::string value;
            while( *position < text.size() )
            {
                const auto current = char_at( text, *position );
                ++*position;

                if( current == '"' )
                {
                    return value;
                }
                if( current != '\\' )
                {
                    value += current;
                    continue;
                }
                if( *position >= text.size() )
                {
                    return std::nullopt;
                }

                const auto escape = char_at( text, *position );
                ++*position;
                switch( escape )
                {
                    case '"' :
                        value += '"';
                        break;
                    case '\\' :
                        value += '\\';
                        break;
                    case 'n' :
                        value += '\n';
                        break;
                    case 't' :
                        value += '\t';
                        break;
                    case 'r' :
                        value += '\r';
                        break;
                    case 'u' :
                        {
                            auto unicode =
                                read_unicode_control_escape( text, *position );
                            if( !unicode.has_value() )
                            {
                                return std::nullopt;
                            }
                            value += *unicode;
                            break;
                        }
                    default :
                        return std::nullopt;
                }
            }

            return std::nullopt;
        }

        [[nodiscard]]
        std::optional<std::string_view>
        find_number( std::string_view text,
                     std::string_view key )
        {
            const auto start = find_value_start( text, key );
            if( !start.has_value() || *start >= text.size() )
            {
                return std::nullopt;
            }

            auto position = *start;
            if( char_at( text, position ) == '-' )
            {
                ++position;
            }

            const auto digit_start = position;
            while( position < text.size() && is_digit( char_at( text, position ) ) )
            {
                ++position;
            }
            if( digit_start == position )
            {
                return std::nullopt;
            }

            const auto value_end = position;
            position             = skip_whitespace( text, position );
            if( position <
                text.size() &&
                char_at( text, position ) !=
                ',' &&
                char_at( text, position ) != '}' )
            {
                return std::nullopt;
            }

            return text.substr( *start, value_end - *start );
        }

        [[nodiscard]]
        std::optional<std::uint64_t>
        parse_uint64_token( std::string_view token )
        {
            if( token.empty() || token.front() == '-' )
            {
                return std::nullopt;
            }

            std::uint64_t value  = 0U;
            const auto*   begin  = token.data();
            const auto*   end    = std::next( begin, std::ssize( token ) );
            const auto    result = std::from_chars( begin, end, value );
            if( result.ec != std::errc{} || result.ptr != end )
            {
                return std::nullopt;
            }
            return value;
        }

        [[nodiscard]]
        std::optional<std::int64_t>
        parse_int64_token( std::string_view token )
        {
            std::int64_t value  = 0;
            const auto*  begin  = token.data();
            const auto*  end    = std::next( begin, std::ssize( token ) );
            const auto   result = std::from_chars( begin, end, value );
            if( result.ec != std::errc{} || result.ptr != end )
            {
                return std::nullopt;
            }
            return value;
        }

        [[nodiscard]]
        std::optional<std::uint64_t>
        find_uint64( std::string_view text,
                     std::string_view key )
        {
            const auto token = find_number( text, key );
            if( !token.has_value() )
            {
                return std::nullopt;
            }
            return parse_uint64_token( *token );
        }

        [[nodiscard]]
        std::optional<std::int64_t>
        find_int64( std::string_view text,
                    std::string_view key )
        {
            const auto token = find_number( text, key );
            if( !token.has_value() )
            {
                return std::nullopt;
            }
            return parse_int64_token( *token );
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
            return grab::fail( ErrorCode::protocol_error, std::move( message ) );
        }

    }    // namespace

    std::string
    to_json( const SessionRecord& record )
    {
        core::json::Writer writer;
        writer.begin_object();
        writer.field( "name", record.name );
        writer.field( "provider", record.provider );
        writer.field( "endpoint", record.endpoint );
        writer.field( "control_socket", record.control_socket );
        writer.field( "mode", mode_name( record.mode ) );
        writer.field( "state", state_name( record.state ) );
        writer.field( "width", static_cast<std::uint64_t>( record.geometry.width ) );
        writer.field( "height", static_cast<std::uint64_t>( record.geometry.height ) );
        writer.field( "supervisor_pid",
                      static_cast<std::int64_t>( record.supervisor_pid ) );
        writer.field( "created_monotonic", record.created_monotonic );
        writer.end_object();
        return std::move( writer ).take();
    }

    grab::Result<SessionRecord>
    parse_record( std::string_view text )
    {
        auto name = find_string( text, name_field );
        if( !name.has_value() )
        {
            return fail_record_field( name_field );
        }

        auto provider = find_string( text, provider_field );
        if( !provider.has_value() )
        {
            return fail_record_field( provider_field );
        }

        auto endpoint = find_string( text, endpoint_field );
        if( !endpoint.has_value() )
        {
            return fail_record_field( endpoint_field );
        }

        auto control_socket = find_string( text, control_socket_field );
        if( !control_socket.has_value() )
        {
            return fail_record_field( control_socket_field );
        }

        const auto mode_text = find_string( text, mode_field );
        if( !mode_text.has_value() )
        {
            return fail_record_field( mode_field );
        }
        const auto mode = mode_from_string( *mode_text );
        if( !mode.has_value() )
        {
            return fail_record_field( mode_field );
        }

        const auto state_text = find_string( text, state_field );
        if( !state_text.has_value() )
        {
            return fail_record_field( state_field );
        }
        const auto state = session_state_from_string( *state_text );
        if( !state.has_value() )
        {
            return fail_record_field( state_field );
        }

        const auto width = find_uint64( text, width_field );
        if( !width.has_value() || !fits_uint16( *width ) )
        {
            return fail_record_field( width_field );
        }

        const auto height = find_uint64( text, height_field );
        if( !height.has_value() || !fits_uint16( *height ) )
        {
            return fail_record_field( height_field );
        }

        const auto supervisor_pid = find_int64( text, supervisor_pid_field );
        if( !supervisor_pid.has_value() )
        {
            return fail_record_field( supervisor_pid_field );
        }

        const auto created_monotonic = find_uint64( text, created_monotonic_field );
        if( !created_monotonic.has_value() )
        {
            return fail_record_field( created_monotonic_field );
        }

        const SessionGeometry geometry{
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
            .supervisor_pid    = *supervisor_pid,
            .created_monotonic = *created_monotonic,
        };
    }

}    // namespace grab::session
