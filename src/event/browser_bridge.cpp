#include "core/reactor.hpp"
#include "event/browser_bridge.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/event_wire.hpp"
#include "grab/pid.hpp"
#include "grab/result.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>    // IWYU pragma: keep
#include <nlohmann/json_fwd.hpp>
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

        constexpr int           kInvalidFd             = -1;
        constexpr int           kNoError               = 0;
        constexpr int           kPosixFailure          = -1;
        constexpr ssize_t       kEndOfFile             = 0;
        constexpr std::uint64_t kNoToken               = 0U;
        constexpr std::uint32_t kNoEvents              = 0U;
        constexpr std::size_t   kFrameHeaderBytes      = 4U;
        constexpr std::size_t   kReadChunkBytes        = 4'096U;
        constexpr std::size_t   kMaxFrameBodyBytes     = 1'048'576U;
        constexpr std::size_t   kLengthByteZeroOffset  = 0U;
        constexpr std::size_t   kLengthByteOneOffset   = 1U;
        constexpr std::size_t   kLengthByteTwoOffset   = 2U;
        constexpr std::size_t   kLengthByteThreeOffset = 3U;
        constexpr unsigned int  kLengthByteOneShift    = 8U;
        constexpr unsigned int  kLengthByteTwoShift    = 16U;
        constexpr unsigned int  kLengthByteThreeShift  = 24U;
        constexpr std::uint32_t kReadableEvents =
            static_cast<std::uint32_t>( EPOLLIN ) |
            static_cast<std::uint32_t>( EPOLLERR ) |
            static_cast<std::uint32_t>( EPOLLHUP );

        constexpr std::string_view kTypeKey         = "type";
        constexpr std::string_view kAppKey          = "app";
        constexpr std::string_view kPidKey          = "pid";
        constexpr std::string_view kTitleKey        = "title";
        constexpr std::string_view kTabTitleKey     = "tab_title";
        constexpr std::string_view kPrevTabTitleKey = "prev_tab_title";
        constexpr std::string_view kDetailKey       = "detail";
        constexpr std::string_view kJsonKey         = "json";
        constexpr std::string_view kTimestampKey    = "timestamp";

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
        is_json_whitespace( char value ) noexcept
        {
            return value == ' ' || value == '\n' || value == '\r' || value == '\t';
        }

        [[nodiscard]]
        std::optional<std::size_t>
        first_non_whitespace( std::string_view input,
                              std::size_t      offset = 0U ) noexcept
        {
            while( offset < input.size() && is_json_whitespace( input.at( offset ) ) )
            {
                ++offset;
            }
            if( offset == input.size() )
            {
                return std::nullopt;
            }
            return offset;
        }

        [[nodiscard]]
        std::string_view
        malformed_json_message( std::string_view input ) noexcept
        {
            const auto object_start = first_non_whitespace( input );
            if( !object_start.has_value() || input.at( *object_start ) != '{' )
            {
                return "expected object start";
            }

            const auto first_member = first_non_whitespace( input, *object_start + 1U );
            if( first_member.has_value() &&
                input.at( *first_member ) !=
                '"' &&
                input.at( *first_member ) != '}' )
            {
                return "expected string";
            }

            return "malformed json";
        }

        [[nodiscard]]
        bool
        has_nested_value( const nlohmann::json& object )
        {
            return std::ranges::any_of( object.items(),
                                        []( const auto& item )
                                        {
                                            return item.value().is_structured();
                                        } );
        }

        [[nodiscard]]
        std::optional<std::string>
        scalar_value( const nlohmann::json& value )
        {
            if( value.is_string() )
            {
                return value.get<std::string>();
            }
            if( value.is_null() || value.is_boolean() || value.is_number() )
            {
                return value.dump();
            }
            return std::nullopt;
        }

        [[nodiscard]]
        std::optional<std::string>
        field_value( const nlohmann::json& object,
                     std::string_view      key )
        {
            const auto member = object.find( std::string{ key } );
            if( member == object.end() )
            {
                return std::nullopt;
            }
            return scalar_value( *member );
        }

        [[nodiscard]]
        std::string
        field_or_empty( const nlohmann::json& object,
                        std::string_view      key )
        {
            const auto value = field_value( object, key );
            if( value.has_value() )
            {
                return *value;
            }
            return {};
        }

        [[nodiscard]]
        std::string
        title_field( const nlohmann::json& object )
        {
            if( const auto title = field_value( object, kTitleKey ); title.has_value() )
            {
                return *title;
            }
            return field_or_empty( object, kTabTitleKey );
        }

        [[nodiscard]]
        std::string
        detail_field( const nlohmann::json& object,
                      std::string_view      type )
        {
            if( const auto detail = field_value( object, kDetailKey );
                detail.has_value() )
            {
                return *detail;
            }
            return std::string{ type };
        }

        [[nodiscard]]
        std::string
        json_field( const nlohmann::json& object,
                    std::string_view      original_json )
        {
            if( const auto json = field_value( object, kJsonKey ); json.has_value() )
            {
                return *json;
            }
            return std::string{ original_json };
        }

        [[nodiscard]]
        std::optional<double>
        timestamp_field( const nlohmann::json& object )
        {
            const auto timestamp = field_value( object, kTimestampKey );
            if( !timestamp.has_value() )
            {
                return std::nullopt;
            }

            double      value  = 0.0;
            const auto* begin  = timestamp->data();
            const auto* end    = begin + timestamp->size();
            const auto  parsed = std::from_chars( begin, end, value );
            if( parsed.ec != std::errc{} || parsed.ptr != end )
            {
                return std::nullopt;
            }
            return value;
        }

        [[nodiscard]]
        grab::Event
        make_browser_tab_event( const nlohmann::json& object )
        {
            const auto kind = grab::EventKind::browser_tab_switched;
            return grab::Event{
                .timestamp = timestamp_field( object ).value_or( 0.0 ),
                .sequence  = 0U,
                .kind      = kind,
                .category  = grab::category_of( kind ),
                .payload   = grab::Payload{ grab::BrowserTab{
                    .app = field_or_empty( object, kAppKey ),
                    .pid = grab::Pid::from_string( field_or_empty( object, kPidKey ) ),
                    .tab_title      = field_or_empty( object, kTabTitleKey ),
                    .prev_tab_title = field_or_empty( object, kPrevTabTitleKey ),
                } },
            };
        }

        [[nodiscard]]
        grab::Event
        make_integration_event( grab::EventKind       kind,
                                const nlohmann::json& object,
                                std::string_view      type,
                                std::string_view      original_json )
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
        auto object = nlohmann::json::parse( json, nullptr, false );
        if( object.is_discarded() )
        {
            return protocol_error( std::string{ malformed_json_message( json ) } );
        }
        if( !object.is_object() )
        {
            return protocol_error( "expected object start" );
        }
        if( has_nested_value( object ) )
        {
            return protocol_error( "nested values are unsupported" );
        }

        const auto type = field_value( object, kTypeKey );
        if( !type.has_value() || type->empty() )
        {
            return protocol_error( "missing message type" );
        }

        const auto kind = grab::wire_kind( *type );

        if( kind == grab::EventKind::browser_tab_switched )
        {
            return make_browser_tab_event( object );
        }

        if( kind == grab::EventKind::app_context_update )
        {
            return make_integration_event( grab::EventKind::app_context_update,
                                           object,
                                           *type,
                                           json );
        }

        // Any other type — including app.tab_changed and wire strings this bridge
        // does not special-case — becomes an app.tab_changed integration event
        // carrying the raw type as its detail.
        return make_integration_event( grab::EventKind::app_tab_changed,
                                       object,
                                       *type,
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
