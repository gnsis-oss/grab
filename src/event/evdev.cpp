#include "core/reactor.hpp"
#include "event/evdev.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/result.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <iterator>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <memory>
#include <mutex>
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

        constexpr int           invalidFd         = -1;
        constexpr int           noError           = 0;
        constexpr int           posixFailure      = -1;
        constexpr int           openFlags         = O_RDONLY | O_NONBLOCK | O_CLOEXEC;
        constexpr int           keyReleaseValue   = 0;
        constexpr int           keyPressValue     = 1;
        constexpr int           keyRepeatValue    = 2;
        constexpr ssize_t       endOfFile         = 0;
        constexpr std::uint64_t noToken           = 0U;
        constexpr std::uint32_t noEvents          = 0U;
        constexpr std::size_t   readChunkBytes    = 4'096U;
        constexpr double        microsecondsInSec = 1'000'000.0;
        constexpr std::uint32_t readableEvents = static_cast<std::uint32_t>( EPOLLIN ) |
                                                 static_cast<std::uint32_t>( EPOLLERR ) |
                                                 static_cast<std::uint32_t>( EPOLLHUP );

        struct ReadResult
        {
                ssize_t bytes_read   = posixFailure;
                int     error_number = noError;
        };

        [[nodiscard]]
        grab::Error
        posix_device_error( std::string_view step,
                            int              error_number )
        {
            return grab::Error{
                .code = grab::ErrorCode::DeviceInaccessible,
                .message =
                    std::string{ step }
                    +
                    ": " +
                    std::error_code{ error_number, std::generic_category() }
                    .message(),
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
        }

        [[nodiscard]]
        grab::Result<void>
        set_nonblocking( int fd )
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX fcntl(2).
            const int flags = ::fcntl( fd, F_GETFL );
            if( flags == posixFailure )
            {
                return std::unexpected( posix_device_error( "fcntl F_GETFL", errno ) );
            }

            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX fcntl(2).
            if( ::fcntl( fd, F_SETFL, flags | O_NONBLOCK ) == posixFailure )
            {
                return std::unexpected( posix_device_error( "fcntl F_SETFL", errno ) );
            }

            return {};
        }

        [[nodiscard]]
        std::string
        axis_name( std::uint16_t code )
        {
            if( code == static_cast<std::uint16_t>( REL_X ) )
            {
                return "x";
            }

            if( code == static_cast<std::uint16_t>( REL_Y ) )
            {
                return "y";
            }

            return std::to_string( code );
        }

        [[nodiscard]]
        double
        event_timestamp( const input_event& raw ) noexcept
        {
            return static_cast<double>( raw.input_event_sec ) +
                   ( static_cast<double>( raw.input_event_usec ) / microsecondsInSec );
        }

        [[nodiscard]]
        grab::Event
        make_key_event( grab::EventKind    kind,
                        const input_event& raw )
        {
            return grab::Event{
                .timestamp = event_timestamp( raw ),
                .sequence  = 0U,
                .kind      = kind,
                .category  = grab::EventCategory::Input,
                .payload   = grab::Payload{ grab::InputKey{
                    .code = raw.code,
                    .name = {},
                } },
            };
        }

        [[nodiscard]]
        grab::Event
        make_motion_event( const input_event& raw )
        {
            return grab::Event{
                .timestamp = event_timestamp( raw ),
                .sequence  = 0U,
                .kind      = grab::EventKind::MouseMove,
                .category  = grab::EventCategory::Input,
                .payload   = grab::Payload{ grab::MouseMove{
                    .axis  = axis_name( raw.code ),
                    .delta = static_cast<double>( raw.value ),
                } },
            };
        }

        void
        append_decoded_event( const input_event&        raw,
                              std::vector<grab::Event>& events )
        {
            switch( raw.type )
            {
                case EV_KEY :
                    if( raw.value == keyPressValue || raw.value == keyRepeatValue )
                    {
                        events.push_back( make_key_event( grab::EventKind::KeyDown,
                                                          raw ) );
                    }
                    else if( raw.value == keyReleaseValue )
                    {
                        events.push_back( make_key_event( grab::EventKind::KeyUp,
                                                          raw ) );
                    }
                    break;
                case EV_REL :
                    events.push_back( make_motion_event( raw ) );
                    break;
                default :
                    break;
            }
        }

        void
        drain_complete_records( std::vector<char>&        buffer,
                                std::vector<grab::Event>& pending_events )
        {
            std::size_t consumed = 0U;
            while( buffer.size() - consumed >= sizeof( input_event ) )
            {
                input_event raw{};
                std::memcpy( &raw,
                             std::next( buffer.data(),
                                        static_cast<std::ptrdiff_t>( consumed ) ),
                             sizeof( input_event ) );
                append_decoded_event( raw, pending_events );
                consumed += sizeof( input_event );
            }

            if( consumed > 0U )
            {
                buffer.erase( buffer.begin(),
                              std::next( buffer.begin(),
                                         static_cast<std::ptrdiff_t>( consumed ) ) );
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

        [[nodiscard]]
        ReadResult
        read_once( int                         input_fd,
                   std::array<char,
                              readChunkBytes>& chunk ) noexcept
        {
            while( true )
            {
                const ssize_t bytes_read =
                    // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection)
                    ::read( input_fd, chunk.data(), chunk.size() );
                const int error_number = errno;
                if( bytes_read != posixFailure || error_number != EINTR )
                {
                    return ReadResult{
                        .bytes_read = bytes_read,
                        .error_number =
                            bytes_read == posixFailure ? error_number : noError,
                    };
                }
            }
        }

    }    // namespace

    struct EvdevMonitor::State
    {
            std::mutex        mutex;
            int               fd  = invalidFd;
            grab::EventBus*   bus = nullptr;
            std::vector<char> buffer;
            bool              active = true;
    };

    EvdevMonitor::EvdevMonitor( grab::core::Reactor&   reactor,
                                std::uint64_t          token,
                                std::shared_ptr<State> state ) noexcept :
        reactor_( &reactor ),
        token_( token ),
        state_( std::move( state ) )
    {
    }

    EvdevMonitor::~EvdevMonitor()
    {
        stop();
    }

    EvdevMonitor::EvdevMonitor( EvdevMonitor&& other ) noexcept :
        reactor_( std::exchange( other.reactor_,
                                 nullptr ) ),
        token_( std::exchange( other.token_,
                               noToken ) ),
        state_( std::move( other.state_ ) )
    {
    }

    EvdevMonitor&
    EvdevMonitor::operator=( EvdevMonitor&& other ) noexcept
    {
        if( this != &other )
        {
            stop();
            reactor_ = std::exchange( other.reactor_, nullptr );
            token_   = std::exchange( other.token_, noToken );
            state_   = std::move( other.state_ );
        }
        return *this;
    }

    grab::Result<EvdevMonitor>
    EvdevMonitor::open_device( const std::string&   path,
                               grab::core::Reactor& reactor,
                               grab::EventBus&      bus )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX open(2).
        const int fd = ::open( path.c_str(), openFlags );
        if( fd == posixFailure )
        {
            return std::unexpected( posix_device_error( "open evdev device", errno ) );
        }

        auto monitor = adopt_fd( fd, reactor, bus );
        if( !monitor.has_value() )
        {
            const auto close_result = ::close( fd );
            static_cast<void>( close_result );
            return std::unexpected( std::move( monitor.error() ) );
        }

        return monitor;
    }

    grab::Result<EvdevMonitor>
    EvdevMonitor::adopt_fd( int                  fd,
                            grab::core::Reactor& reactor,
                            grab::EventBus&      bus )
    {
        if( fd == invalidFd )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument, "evdev fd is invalid" );
        }

        auto nonblocking = set_nonblocking( fd );
        if( !nonblocking.has_value() )
        {
            return std::unexpected( std::move( nonblocking.error() ) );
        }

        auto state = std::make_shared<State>();
        state->fd  = fd;
        state->bus = &bus;

        const auto token =
            reactor.add_fd( fd,
                            static_cast<std::uint32_t>( EPOLLIN ),
                            [state]( std::uint32_t event_mask )
                            {
                                EvdevMonitor::handle_fd( state, event_mask );
                            } );

        return EvdevMonitor{ reactor, token, std::move( state ) };
    }

    void
    EvdevMonitor::handle_fd( const std::shared_ptr<State>& state,
                             std::uint32_t                 events )
    {
        if( ( events & readableEvents ) == noEvents )
        {
            return;
        }

        std::vector<grab::Event> pending_events;
        grab::EventBus*          bus = nullptr;
        {
            const std::scoped_lock lock( state->mutex );
            if( !state->active || state->fd == invalidFd || state->bus == nullptr )
            {
                return;
            }

            bus = state->bus;

            std::array<char, readChunkBytes> chunk{};
            while( true )
            {
                const ReadResult read_result = read_once( state->fd, chunk );
                if( read_result.bytes_read == endOfFile )
                {
                    state->active = false;
                    break;
                }

                if( read_result.bytes_read == posixFailure )
                {
                    if( read_result.error_number !=
                        EAGAIN &&
                        read_result.error_number != EWOULDBLOCK )
                    {
                        state->active = false;
                        state->buffer.clear();
                    }
                    break;
                }

                state->buffer.insert(
                    state->buffer.end(),
                    chunk.begin(),
                    std::next( chunk.begin(),
                               static_cast<std::ptrdiff_t>( read_result.bytes_read ) )
                );
                drain_complete_records( state->buffer, pending_events );
            }

            if( ( events & static_cast<std::uint32_t>( EPOLLERR | EPOLLHUP ) ) !=
                noEvents )
            {
                state->active = false;
            }

            if( !state->active )
            {
                state->buffer.clear();
            }
        }

        if( bus != nullptr )
        {
            publish_pending_events( *bus, pending_events );
        }
    }

    void
    EvdevMonitor::stop() noexcept
    {
        grab::core::Reactor* const reactor = std::exchange( reactor_, nullptr );
        const std::uint64_t        token   = std::exchange( token_, noToken );
        auto                       state   = std::move( state_ );

        if( reactor != nullptr && token != noToken )
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

        int fd = invalidFd;
        try
        {
            const std::scoped_lock lock( state->mutex );
            state->active = false;
            state->bus    = nullptr;
            fd            = std::exchange( state->fd, invalidFd );
            state->buffer.clear();
        }
        catch( ... )
        {
            return;
        }

        if( fd != invalidFd )
        {
            const auto close_result = ::close( fd );
            static_cast<void>( close_result );
        }
    }

}    // namespace grab::event
