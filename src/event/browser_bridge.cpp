#include "core/reactor.hpp"
#include "event/browser_bridge.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/result.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/epoll.h>    // IWYU pragma: keep
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace grab::event
{
    namespace
    {

        constexpr int           kInvalidFd              = -1;
        constexpr int           kNoError                = 0;
        constexpr int           kPosixFailure           = -1;
        constexpr ssize_t       kEndOfFile              = 0;
        constexpr std::uint64_t kNoToken                = 0U;
        constexpr std::uint32_t kNoEvents               = 0U;
        constexpr std::size_t   kFrameHeaderBytes       = 4U;
        constexpr std::size_t   kReadChunkBytes         = 4'096U;
        constexpr std::size_t   kMaxFrameBodyBytes      = 1'048'576U;
        constexpr std::size_t   kLengthByteZeroOffset   = 0U;
        constexpr std::size_t   kLengthByteOneOffset    = 1U;
        constexpr std::size_t   kLengthByteTwoOffset    = 2U;
        constexpr std::size_t   kLengthByteThreeOffset  = 3U;
        constexpr unsigned int  kLengthByteOneShift     = 8U;
        constexpr unsigned int  kLengthByteTwoShift     = 16U;
        constexpr unsigned int  kLengthByteThreeShift   = 24U;
        constexpr std::uint32_t kUnicodeEscapeDigits    = 4U;
        constexpr std::uint32_t kHexDigitBits           = 4U;
        constexpr std::uint32_t kHighSurrogateMin       = 0XD8'00U;
        constexpr std::uint32_t kHighSurrogateMax       = 0XDB'FFU;
        constexpr std::uint32_t kLowSurrogateMin        = 0XDC'00U;
        constexpr std::uint32_t kLowSurrogateMax        = 0XDF'FFU;
        constexpr std::uint32_t kSurrogateShift         = 10U;
        constexpr std::uint32_t kSupplementaryPlaneBase = 0X1'00'00U;
        constexpr std::uint32_t kUtf8OneByteMax         = 0X7FU;
        constexpr std::uint32_t kUtf8TwoByteMax         = 0X7'FFU;
        constexpr std::uint32_t kUtf8ThreeByteMax       = 0XFF'FFU;
        constexpr std::uint32_t kUtf8MaxCodePoint       = 0X10'FF'FFU;
        constexpr std::uint32_t kUtf8ContinuationMask   = 0X3FU;
        constexpr std::uint32_t kUtf8ContinuationTag    = 0X80U;
        constexpr std::uint32_t kUtf8TwoByteTag         = 0XC0U;
        constexpr std::uint32_t kUtf8ThreeByteTag       = 0XE0U;
        constexpr std::uint32_t kUtf8FourByteTag        = 0XF0U;
        constexpr unsigned int  kUtf8OneContinuation    = 6U;
        constexpr unsigned int  kUtf8TwoContinuations   = 12U;
        constexpr unsigned int  kUtf8ThreeContinuations = 18U;
        constexpr unsigned char kJsonControlMax         = 0X1FU;
        constexpr std::uint32_t kReadableEvents =
            static_cast<std::uint32_t>( EPOLLIN ) |
            static_cast<std::uint32_t>( EPOLLERR ) |
            static_cast<std::uint32_t>( EPOLLHUP );

        constexpr std::string_view kTypeKey                = "type";
        constexpr std::string_view kAppKey                 = "app";
        constexpr std::string_view kPidKey                 = "pid";
        constexpr std::string_view kTitleKey               = "title";
        constexpr std::string_view kTabTitleKey            = "tab_title";
        constexpr std::string_view kPrevTabTitleKey        = "prev_tab_title";
        constexpr std::string_view kDetailKey              = "detail";
        constexpr std::string_view kJsonKey                = "json";
        constexpr std::string_view kTimestampKey           = "timestamp";
        constexpr std::string_view kTabSwitchedType        = "tab_switched";
        constexpr std::string_view kBrowserTabSwitchedType = "browser.tab_switched";
        constexpr std::string_view kContextUpdateType      = "context_update";
        constexpr std::string_view kAppContextUpdateType   = "app.context_update";
        constexpr std::string_view kAppTabChangedType      = "app.tab_changed";

        struct JsonMember
        {
                std::string key;
                std::string value;
        };

        struct JsonObject
        {
                std::vector<JsonMember> members;
        };

        struct ReadResult
        {
                ssize_t bytes_read   = kPosixFailure;
                int     error_number = kNoError;
        };

        [[nodiscard]]
        std::unexpected<grab::Error>
        protocol_error( std::string message )
        {
            return grab::fail( grab::ErrorCode::protocol_error,
                               "browser bridge: " + std::move( message ) );
        }

        [[nodiscard]]
        bool
        is_whitespace( char value ) noexcept
        {
            return value == ' ' || value == '\n' || value == '\r' || value == '\t';
        }

        [[nodiscard]]
        bool
        is_digit( char value ) noexcept
        {
            return value >= '0' && value <= '9';
        }

        [[nodiscard]]
        bool
        is_nonzero_digit( char value ) noexcept
        {
            return value >= '1' && value <= '9';
        }

        [[nodiscard]]
        bool
        is_hex_digit( char value ) noexcept
        {
            return ( value >= '0' && value <= '9' ) ||
                   ( value >= 'a' && value <= 'f' ) ||
                   ( value >= 'A' && value <= 'F' );
        }

        [[nodiscard]]
        std::uint32_t
        hex_value( char value ) noexcept
        {
            if( value >= '0' && value <= '9' )
            {
                return static_cast<std::uint32_t>( value - '0' );
            }
            if( value >= 'a' && value <= 'f' )
            {
                return static_cast<std::uint32_t>( value - 'a' ) +
                       static_cast<std::uint32_t>( '9' - '0' ) +
                       1U;
            }
            return static_cast<std::uint32_t>( value - 'A' ) +
                   static_cast<std::uint32_t>( '9' - '0' ) +
                   1U;
        }

        [[nodiscard]]
        bool
        is_high_surrogate( std::uint32_t value ) noexcept
        {
            return value >= kHighSurrogateMin && value <= kHighSurrogateMax;
        }

        [[nodiscard]]
        bool
        is_low_surrogate( std::uint32_t value ) noexcept
        {
            return value >= kLowSurrogateMin && value <= kLowSurrogateMax;
        }

        [[nodiscard]]
        bool
        is_valid_number_literal( std::string_view literal ) noexcept
        {
            if( literal.empty() )
            {
                return false;
            }

            std::size_t offset = 0U;
            if( literal.at( offset ) == '-' )
            {
                ++offset;
                if( offset == literal.size() )
                {
                    return false;
                }
            }

            if( literal.at( offset ) == '0' )
            {
                ++offset;
            }
            else
            {
                if( !is_nonzero_digit( literal.at( offset ) ) )
                {
                    return false;
                }
                while( offset < literal.size() && is_digit( literal.at( offset ) ) )
                {
                    ++offset;
                }
            }

            if( offset < literal.size() && literal.at( offset ) == '.' )
            {
                ++offset;
                if( offset == literal.size() || !is_digit( literal.at( offset ) ) )
                {
                    return false;
                }
                while( offset < literal.size() && is_digit( literal.at( offset ) ) )
                {
                    ++offset;
                }
            }

            if( offset <
                literal.size() &&
                ( literal.at( offset ) == 'e' || literal.at( offset ) == 'E' ) )
            {
                ++offset;
                if( offset <
                    literal.size() &&
                    ( literal.at( offset ) == '-' || literal.at( offset ) == '+' ) )
                {
                    ++offset;
                }
                if( offset == literal.size() || !is_digit( literal.at( offset ) ) )
                {
                    return false;
                }
                while( offset < literal.size() && is_digit( literal.at( offset ) ) )
                {
                    ++offset;
                }
            }

            return offset == literal.size();
        }

        [[nodiscard]]
        bool
        is_valid_literal( std::string_view literal ) noexcept
        {
            return literal ==
                   "true" ||
                   literal ==
                   "false" ||
                   literal ==
                   "null" ||
                   is_valid_number_literal( literal );
        }

        [[nodiscard]]
        std::string_view
        trim( std::string_view value ) noexcept
        {
            while( !value.empty() && is_whitespace( value.front() ) )
            {
                value.remove_prefix( 1U );
            }
            while( !value.empty() && is_whitespace( value.back() ) )
            {
                value.remove_suffix( 1U );
            }
            return value;
        }

        [[nodiscard]]
        char
        utf8_byte( std::uint32_t value ) noexcept
        {
            return static_cast<char>( value );
        }

        [[nodiscard]]
        char
        utf8_continuation( std::uint32_t value ) noexcept
        {
            return utf8_byte( kUtf8ContinuationTag | ( value & kUtf8ContinuationMask ) );
        }

        [[nodiscard]]
        grab::Result<void>
        append_utf8( std::string&  output,
                     std::uint32_t code_point )
        {
            if( code_point <= kUtf8OneByteMax )
            {
                output.push_back( utf8_byte( code_point ) );
                return {};
            }

            if( code_point <= kUtf8TwoByteMax )
            {
                output.push_back( utf8_byte( kUtf8TwoByteTag |
                                             ( code_point >> kUtf8OneContinuation ) ) );
                output.push_back( utf8_continuation( code_point ) );
                return {};
            }

            if( code_point >= kHighSurrogateMin && code_point <= kLowSurrogateMax )
            {
                return protocol_error( "invalid unicode surrogate" );
            }

            if( code_point <= kUtf8ThreeByteMax )
            {
                output.push_back( utf8_byte( kUtf8ThreeByteTag |
                                             ( code_point >> kUtf8TwoContinuations ) ) );
                output.push_back( utf8_continuation( code_point >>
                                                     kUtf8OneContinuation ) );
                output.push_back( utf8_continuation( code_point ) );
                return {};
            }

            if( code_point <= kUtf8MaxCodePoint )
            {
                output.push_back( utf8_byte(
                    kUtf8FourByteTag | ( code_point >> kUtf8ThreeContinuations )
                ) );
                output.push_back( utf8_continuation( code_point >>
                                                     kUtf8TwoContinuations ) );
                output.push_back( utf8_continuation( code_point >>
                                                     kUtf8OneContinuation ) );
                output.push_back( utf8_continuation( code_point ) );
                return {};
            }

            return protocol_error( "unicode code point is out of range" );
        }

        class FlatJsonParser
        {
            public:

                explicit FlatJsonParser( std::string_view input ) noexcept :
                    input_( input )
                {
                }

                [[nodiscard]]
                grab::Result<JsonObject>
                parse_object()
                {
                    skip_whitespace();
                    if( auto consumed = consume( '{', "expected object start" );
                        !consumed.has_value() )
                    {
                        return std::unexpected( std::move( consumed.error() ) );
                    }
                    skip_whitespace();

                    JsonObject object;
                    if( try_consume( '}' ) )
                    {
                        skip_whitespace();
                        if( !at_end() )
                        {
                            return protocol_error( "trailing data after object" );
                        }
                        return object;
                    }

                    while( true )
                    {
                        auto key = parse_string();
                        if( !key.has_value() )
                        {
                            return std::unexpected( std::move( key.error() ) );
                        }
                        skip_whitespace();
                        if( auto consumed = consume( ':', "expected object colon" );
                            !consumed.has_value() )
                        {
                            return std::unexpected( std::move( consumed.error() ) );
                        }
                        skip_whitespace();

                        auto value = parse_value();
                        if( !value.has_value() )
                        {
                            return std::unexpected( std::move( value.error() ) );
                        }
                        object.members.push_back( JsonMember{
                            .key   = std::move( *key ),
                            .value = std::move( *value ),
                        } );
                        skip_whitespace();

                        if( try_consume( ',' ) )
                        {
                            skip_whitespace();
                            continue;
                        }
                        if( try_consume( '}' ) )
                        {
                            break;
                        }
                        return protocol_error( "expected comma or object end" );
                    }

                    skip_whitespace();
                    if( !at_end() )
                    {
                        return protocol_error( "trailing data after object" );
                    }
                    return object;
                }

            private:

                [[nodiscard]]
                bool
                at_end() const noexcept
                {
                    return offset_ == input_.size();
                }

                [[nodiscard]]
                char
                peek() const noexcept
                {
                    return input_.at( offset_ );
                }

                void
                skip_whitespace() noexcept
                {
                    while( offset_ <
                           input_.size() &&
                           is_whitespace( input_.at( offset_ ) ) )
                    {
                        ++offset_;
                    }
                }

                [[nodiscard]]
                bool
                try_consume( char expected ) noexcept
                {
                    if( offset_ < input_.size() && input_.at( offset_ ) == expected )
                    {
                        ++offset_;
                        return true;
                    }
                    return false;
                }

                [[nodiscard]]
                grab::Result<void>
                consume( char             expected,
                         std::string_view message )
                {
                    if( try_consume( expected ) )
                    {
                        return {};
                    }
                    return protocol_error( std::string{ message } );
                }

                [[nodiscard]]
                grab::Result<std::uint32_t>
                parse_hex4()
                {
                    std::uint32_t value = 0U;
                    for( std::uint32_t index = 0U; index < kUnicodeEscapeDigits;
                         ++index )
                    {
                        if( at_end() || !is_hex_digit( peek() ) )
                        {
                            return protocol_error( "invalid unicode escape" );
                        }
                        value = ( value << kHexDigitBits ) + hex_value( peek() );
                        ++offset_;
                    }
                    return value;
                }

                [[nodiscard]]
                grab::Result<std::uint32_t>
                parse_unicode_escape()
                {
                    auto code_point = parse_hex4();
                    if( !code_point.has_value() )
                    {
                        return std::unexpected( std::move( code_point.error() ) );
                    }

                    if( !is_high_surrogate( *code_point ) )
                    {
                        if( is_low_surrogate( *code_point ) )
                        {
                            return protocol_error(
                                "low surrogate without high surrogate"
                            );
                        }
                        return *code_point;
                    }

                    if( !try_consume( '\\' ) || !try_consume( 'u' ) )
                    {
                        return protocol_error( "missing low surrogate" );
                    }
                    auto low = parse_hex4();
                    if( !low.has_value() )
                    {
                        return std::unexpected( std::move( low.error() ) );
                    }
                    if( !is_low_surrogate( *low ) )
                    {
                        return protocol_error( "invalid low surrogate" );
                    }

                    return kSupplementaryPlaneBase +
                           ( ( *code_point - kHighSurrogateMin ) << kSurrogateShift ) +
                           ( *low - kLowSurrogateMin );
                }

                [[nodiscard]]
                grab::Result<void>
                append_escape( std::string& output )
                {
                    if( at_end() )
                    {
                        return protocol_error( "unterminated escape" );
                    }

                    const char escape = input_.at( offset_ );
                    ++offset_;
                    switch( escape )
                    {
                        case '"' :
                            output.push_back( '"' );
                            return {};
                        case '\\' :
                            output.push_back( '\\' );
                            return {};
                        case '/' :
                            output.push_back( '/' );
                            return {};
                        case 'b' :
                            output.push_back( '\b' );
                            return {};
                        case 'f' :
                            output.push_back( '\f' );
                            return {};
                        case 'n' :
                            output.push_back( '\n' );
                            return {};
                        case 'r' :
                            output.push_back( '\r' );
                            return {};
                        case 't' :
                            output.push_back( '\t' );
                            return {};
                        case 'u' :
                            {
                                auto code_point = parse_unicode_escape();
                                if( !code_point.has_value() )
                                {
                                    return std::unexpected(
                                        std::move( code_point.error() )
                                    );
                                }
                                return append_utf8( output, *code_point );
                            }
                        default :
                            return protocol_error( "unknown escape" );
                    }
                }

                [[nodiscard]]
                grab::Result<std::string>
                parse_string()
                {
                    if( auto consumed = consume( '"', "expected string" );
                        !consumed.has_value() )
                    {
                        return std::unexpected( std::move( consumed.error() ) );
                    }

                    std::string output;
                    while( !at_end() )
                    {
                        const auto value =
                            static_cast<unsigned char>( input_.at( offset_ ) );
                        ++offset_;
                        if( value == static_cast<unsigned char>( '"' ) )
                        {
                            return output;
                        }
                        if( value == static_cast<unsigned char>( '\\' ) )
                        {
                            if( auto escaped = append_escape( output );
                                !escaped.has_value() )
                            {
                                return std::unexpected( std::move( escaped.error() ) );
                            }
                            continue;
                        }
                        if( value <= kJsonControlMax )
                        {
                            return protocol_error( "control character in string" );
                        }
                        output.push_back( static_cast<char>( value ) );
                    }

                    return protocol_error( "unterminated string" );
                }

                [[nodiscard]]
                grab::Result<std::string>
                parse_literal()
                {
                    const std::size_t start = offset_;
                    while( offset_ <
                           input_.size() &&
                           input_.at( offset_ ) !=
                           ',' &&
                           input_.at( offset_ ) != '}' )
                    {
                        if( input_.at( offset_ ) ==
                            '{' ||
                            input_.at( offset_ ) ==
                            '[' ||
                            input_.at( offset_ ) ==
                            ']' ||
                            input_.at( offset_ ) == '"' )
                        {
                            return protocol_error( "invalid literal" );
                        }
                        ++offset_;
                    }

                    const auto literal = trim( input_.substr( start, offset_ - start ) );
                    if( literal.empty() || !is_valid_literal( literal ) )
                    {
                        return protocol_error( "invalid literal" );
                    }
                    return std::string{ literal };
                }

                [[nodiscard]]
                grab::Result<std::string>
                parse_value()
                {
                    if( at_end() )
                    {
                        return protocol_error( "expected value" );
                    }
                    if( peek() == '"' )
                    {
                        return parse_string();
                    }
                    if( peek() == '{' || peek() == '[' )
                    {
                        return protocol_error( "nested values are unsupported" );
                    }
                    return parse_literal();
                }

                std::string_view input_;
                std::size_t      offset_ = 0U;
        };

        [[nodiscard]]
        std::optional<std::string_view>
        field_value( const JsonObject& object,
                     std::string_view  key ) noexcept
        {
            for( const auto& member : object.members )
            {
                if( member.key == key )
                {
                    return std::string_view{ member.value };
                }
            }
            return std::nullopt;
        }

        [[nodiscard]]
        std::string
        field_or_empty( const JsonObject& object,
                        std::string_view  key )
        {
            const auto value = field_value( object, key );
            if( value.has_value() )
            {
                return std::string{ *value };
            }
            return {};
        }

        [[nodiscard]]
        std::string
        title_field( const JsonObject& object )
        {
            if( const auto title = field_value( object, kTitleKey ); title.has_value() )
            {
                return std::string{ *title };
            }
            return field_or_empty( object, kTabTitleKey );
        }

        [[nodiscard]]
        std::string
        detail_field( const JsonObject& object,
                      std::string_view  type )
        {
            if( const auto detail = field_value( object, kDetailKey );
                detail.has_value() )
            {
                return std::string{ *detail };
            }
            return std::string{ type };
        }

        [[nodiscard]]
        std::string
        json_field( const JsonObject& object,
                    std::string_view  original_json )
        {
            if( const auto json = field_value( object, kJsonKey ); json.has_value() )
            {
                return std::string{ *json };
            }
            return std::string{ original_json };
        }

        [[nodiscard]]
        std::optional<double>
        timestamp_field( const JsonObject& object ) noexcept
        {
            const auto timestamp = field_value( object, kTimestampKey );
            if( !timestamp.has_value() )
            {
                return std::nullopt;
            }

            double     value = 0.0;
            const auto parsed =
                std::from_chars( timestamp->begin(), timestamp->end(), value );
            if( parsed.ec != std::errc{} || parsed.ptr != timestamp->end() )
            {
                return std::nullopt;
            }
            return value;
        }

        [[nodiscard]]
        grab::Event
        make_browser_tab_event( const JsonObject& object )
        {
            const auto kind = grab::EventKind::browser_tab_switched;
            return grab::Event{
                .timestamp = timestamp_field( object ).value_or( 0.0 ),
                .sequence  = 0U,
                .kind      = kind,
                .category  = grab::category_of( kind ),
                .payload   = grab::Payload{ grab::BrowserTab{
                    .app            = field_or_empty( object, kAppKey ),
                    .pid            = field_or_empty( object, kPidKey ),
                    .tab_title      = field_or_empty( object, kTabTitleKey ),
                    .prev_tab_title = field_or_empty( object, kPrevTabTitleKey ),
                } },
            };
        }

        [[nodiscard]]
        grab::Event
        make_integration_event( grab::EventKind   kind,
                                const JsonObject& object,
                                std::string_view  type,
                                std::string_view  original_json )
        {
            return grab::Event{
                .timestamp = timestamp_field( object ).value_or( 0.0 ),
                .sequence  = 0U,
                .kind      = kind,
                .category  = grab::category_of( kind ),
                .payload   = grab::Payload{ grab::IntegrationEvent{
                    .app    = field_or_empty( object, kAppKey ),
                    .title  = title_field( object ),
                    .detail = detail_field( object, type ),
                    .json   = json_field( object, original_json ),
                } },
            };
        }

        [[nodiscard]]
        std::uint32_t
        byte_at( const std::vector<char>& bytes,
                 std::size_t              offset ) noexcept
        {
            return static_cast<std::uint32_t>(
                static_cast<unsigned char>( bytes.at( offset ) )
            );
        }

        [[nodiscard]]
        std::uint32_t
        frame_body_length( const std::vector<char>& bytes,
                           std::size_t              offset ) noexcept
        {
            return byte_at( bytes, offset + kLengthByteZeroOffset ) |
                   ( byte_at( bytes, offset + kLengthByteOneOffset )
                     << kLengthByteOneShift ) |
                   ( byte_at( bytes, offset + kLengthByteTwoOffset )
                     << kLengthByteTwoShift ) |
                   ( byte_at( bytes, offset + kLengthByteThreeOffset )
                     << kLengthByteThreeShift );
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

        [[nodiscard]]
        ReadResult
        read_once( int                          input_fd,
                   std::array<char,
                              kReadChunkBytes>& chunk ) noexcept
        {
            while( true )
            {
                const ssize_t bytes_read =
                    ::read( input_fd, chunk.data(), chunk.size() );
                const int error_number = errno;
                if( bytes_read != kPosixFailure || error_number != EINTR )
                {
                    return ReadResult{
                        .bytes_read = bytes_read,
                        .error_number =
                            bytes_read == kPosixFailure ? error_number : kNoError,
                    };
                }
            }
        }

    }    // namespace

    struct BrowserBridge::State
    {
            std::mutex        mutex;
            int               input_fd = kInvalidFd;
            grab::EventBus*   bus      = nullptr;
            std::vector<char> buffer;
            bool              active = true;
    };

    grab::Result<grab::Event>
    parse_browser_message( std::string_view json )
    {
        FlatJsonParser parser{ json };
        auto           object = parser.parse_object();
        if( !object.has_value() )
        {
            return std::unexpected( std::move( object.error() ) );
        }

        const auto type = field_value( *object, kTypeKey );
        if( !type.has_value() || type->empty() )
        {
            return protocol_error( "missing message type" );
        }

        if( *type == kTabSwitchedType || *type == kBrowserTabSwitchedType )
        {
            return make_browser_tab_event( *object );
        }

        if( *type == kContextUpdateType || *type == kAppContextUpdateType )
        {
            return make_integration_event( grab::EventKind::app_context_update,
                                           *object,
                                           *type,
                                           json );
        }

        return make_integration_event( grab::EventKind::app_tab_changed,
                                       *object,
                                       *type == kAppTabChangedType
                                           ? std::string_view{ kAppTabChangedType }
                                           : *type,
                                       json );
    }

    BrowserBridge::BrowserBridge( grab::core::Reactor&   reactor,
                                  std::uint64_t          token,
                                  std::shared_ptr<State> state ) noexcept :
        reactor_( &reactor ),
        token_( token ),
        state_( std::move( state ) )
    {
    }

    BrowserBridge::~BrowserBridge()
    {
        stop();
    }

    BrowserBridge::BrowserBridge( BrowserBridge&& other ) noexcept :
        reactor_( std::exchange( other.reactor_,
                                 nullptr ) ),
        token_( std::exchange( other.token_,
                               kNoToken ) ),
        state_( std::move( other.state_ ) )
    {
    }

    BrowserBridge&
    BrowserBridge::operator=( BrowserBridge&& other ) noexcept
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

    grab::Result<BrowserBridge>
    BrowserBridge::start( int                  input_fd,
                          grab::core::Reactor& reactor,
                          grab::EventBus&      bus )
    {
        if( input_fd == kInvalidFd )
        {
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "browser bridge input fd is invalid" );
        }

        auto state      = std::make_shared<State>();
        state->input_fd = input_fd;
        state->bus      = &bus;

        const auto token =
            reactor.add_fd( input_fd,
                            static_cast<std::uint32_t>( EPOLLIN ),
                            [state]( std::uint32_t event_mask )
                            {
                                BrowserBridge::handle_fd( state, event_mask );
                            } );

        return BrowserBridge{ reactor, token, std::move( state ) };
    }

    int
    BrowserBridge::active_input_fd( const std::shared_ptr<State>& state )
    {
        const std::scoped_lock lock( state->mutex );
        if( !state->active || state->input_fd == kInvalidFd || state->bus == nullptr )
        {
            return kInvalidFd;
        }
        return state->input_fd;
    }

    void
    BrowserBridge::append_read_bytes( State&                state,
                                      std::span<const char> bytes )
    {
        state.buffer.insert( state.buffer.end(), bytes.begin(), bytes.end() );
    }

    void
    BrowserBridge::drain_complete_frames( State&                    state,
                                          std::vector<grab::Event>& pending_events )
    {
        std::size_t consumed = 0U;
        while( state.buffer.size() - consumed >= kFrameHeaderBytes )
        {
            const std::uint32_t body_length =
                frame_body_length( state.buffer, consumed );
            if( body_length > kMaxFrameBodyBytes )
            {
                state.active = false;
                state.buffer.clear();
                consumed = 0U;
                break;
            }

            const std::size_t frame_size =
                kFrameHeaderBytes + static_cast<std::size_t>( body_length );
            if( state.buffer.size() - consumed < frame_size )
            {
                break;
            }

            const auto body_begin =
                std::next( state.buffer.begin(),
                           static_cast<std::ptrdiff_t>( consumed + kFrameHeaderBytes ) );
            const std::string body{
                body_begin,
                std::next( body_begin, static_cast<std::ptrdiff_t>( body_length ) ),
            };
            auto parsed = parse_browser_message( body );
            if( parsed.has_value() )
            {
                pending_events.push_back( std::move( *parsed ) );
            }
            consumed += frame_size;
        }

        if( consumed > 0U )
        {
            state.buffer.erase( state.buffer.begin(),
                                std::next( state.buffer.begin(),
                                           static_cast<std::ptrdiff_t>( consumed ) ) );
        }
    }

    void
    BrowserBridge::apply_terminal_events( State&        state,
                                          std::uint32_t events ) noexcept
    {
        if( ( events & static_cast<std::uint32_t>( EPOLLHUP | EPOLLERR ) ) != kNoEvents )
        {
            state.active = false;
        }
        if( !state.active )
        {
            state.buffer.clear();
        }
    }

    void
    BrowserBridge::handle_fd( const std::shared_ptr<State>& state,
                              std::uint32_t                 events )
    {
        if( ( events & kReadableEvents ) == kNoEvents )
        {
            return;
        }

        std::vector<grab::Event> pending_events;
        const int                input_fd = active_input_fd( state );
        if( input_fd == kInvalidFd )
        {
            return;
        }

        std::array<char, kReadChunkBytes> chunk{};
        const ReadResult                  read_result = read_once( input_fd, chunk );

        grab::EventBus*                   bus         = nullptr;
        {
            const std::scoped_lock lock( state->mutex );
            if( !state->active || state->bus == nullptr )
            {
                return;
            }

            bus = state->bus;

            if( read_result.bytes_read == kEndOfFile )
            {
                state->active = false;
            }
            else if( read_result.bytes_read == kPosixFailure )
            {
                if( read_result.error_number !=
                    EAGAIN &&
                    read_result.error_number != EWOULDBLOCK )
                {
                    state->active = false;
                }
            }
            else
            {
                append_read_bytes( *state,
                                   std::span{
                                       chunk.data(),
                                       static_cast<std::size_t>( read_result.bytes_read )
                                   } );
                drain_complete_frames( *state, pending_events );
            }

            apply_terminal_events( *state, events );
        }

        if( bus != nullptr )
        {
            publish_pending_events( *bus, pending_events );
        }
    }

    void
    BrowserBridge::stop() noexcept
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

        try
        {
            const std::scoped_lock lock( state->mutex );
            state->active   = false;
            state->bus      = nullptr;
            state->input_fd = kInvalidFd;
            state->buffer.clear();
        }
        catch( ... )
        {
            return;
        }
    }

}    // namespace grab::event
