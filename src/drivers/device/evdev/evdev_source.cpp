#include "drivers/device/evdev/evdev_source.hpp"
#include "grab/context.hpp"
#include "grab/event.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/result.hpp"
#include "spi/event_source.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <functional>
#include <iterator>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <set>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace grab::drivers::device::evdev
{
    namespace
    {

        constexpr int         invalidFd         = -1;
        constexpr int         noError           = 0;
        constexpr int         posixFailure      = -1;
        constexpr int         openFlags         = O_RDONLY | O_NONBLOCK | O_CLOEXEC;
        constexpr int         keyReleaseValue   = 0;
        constexpr int         keyPressValue     = 1;
        constexpr int         keyRepeatValue    = 2;
        constexpr ssize_t     endOfFile         = 0;
        constexpr std::size_t readChunkBytes    = 4'096U;
        constexpr double      microsecondsInSec = 1'000'000.0;
        constexpr decltype( pollfd::revents ) noPollEvents            = 0;
        constexpr std::size_t                 pollTargetCount         = 1U;
        constexpr std::int64_t                minimumPollMilliseconds = 1;
        constexpr std::int64_t                maximumPollMilliseconds = 1'000;

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

        void
        drain_available( int                       input_fd,
                         std::vector<char>&        buffer,
                         bool&                     active,
                         std::vector<grab::Event>& pending )
        {
            if( !active )
            {
                return;
            }

            std::array<char, readChunkBytes> chunk{};
            while( true )
            {
                const ReadResult read_result = read_once( input_fd, chunk );
                if( read_result.bytes_read == endOfFile )
                {
                    active = false;
                    break;
                }

                if( read_result.bytes_read == posixFailure )
                {
                    if( read_result.error_number !=
                        EAGAIN &&
                        read_result.error_number != EWOULDBLOCK )
                    {
                        active = false;
                        buffer.clear();
                    }
                    break;
                }

                buffer.insert(
                    buffer.end(),
                    chunk.begin(),
                    std::next( chunk.begin(),
                               static_cast<std::ptrdiff_t>( read_result.bytes_read ) )
                );
                drain_complete_records( buffer, pending );
            }
        }

    }    // namespace

    EvdevSource::EvdevSource( int fd ) noexcept :
        fd_{ fd }
    {
    }

    EvdevSource::~EvdevSource()
    {
        if( fd_ != invalidFd )
        {
            const auto close_result = ::close( fd_ );
            static_cast<void>( close_result );
        }
    }

    grab::Result<std::unique_ptr<EvdevSource>>
    EvdevSource::open_device( const std::string& path )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX open(2).
        const int fd = ::open( path.c_str(), openFlags );
        if( fd == posixFailure )
        {
            return std::unexpected( posix_device_error( "open evdev device", errno ) );
        }

        auto source = adopt_fd( fd );
        if( !source )
        {
            const auto close_result = ::close( fd );
            static_cast<void>( close_result );
            return std::unexpected( std::move( source.error() ) );
        }

        return source;
    }

    grab::Result<std::unique_ptr<EvdevSource>>
    EvdevSource::adopt_fd( int fd )
    {
        if( fd == invalidFd )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument, "evdev fd is invalid" );
        }

        auto nonblocking = set_nonblocking( fd );
        if( !nonblocking )
        {
            return std::unexpected( std::move( nonblocking.error() ) );
        }

        return std::unique_ptr<EvdevSource>{ new EvdevSource{ fd } };
    }

    void
    EvdevSource::set_sink( EventSink sink )
    {
        std::scoped_lock lock{ sink_mutex_ };
        sink_ = std::move( sink );
    }

    grab::Result<void>
    EvdevSource::enable( const grab::spi::EventSpec& spec )
    {
        if( spec.name.empty() )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "evdev event name is empty" );
        }

        const std::scoped_lock lock{ state_mutex_ };
        enabled_.insert( spec.name );
        return {};
    }

    grab::Result<void>
    EvdevSource::disable( const grab::spi::EventSpec& spec )
    {
        const std::scoped_lock lock{ state_mutex_ };
        enabled_.erase( spec.name );
        return {};
    }

    grab::Result<void>
    EvdevSource::wait_for_event( const grab::spi::EventSpec&   spec,
                                 const grab::OperationContext& context,
                                 std::chrono::nanoseconds      maximum_wait )
    {
        if( maximum_wait < std::chrono::nanoseconds::zero() )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "evdev maximum wait is negative" );
        }

        auto checked = context.check();
        if( !checked )
        {
            return std::unexpected( std::move( checked.error() ) );
        }

        const auto wake_at = std::chrono::steady_clock::now() +
                             std::min( maximum_wait, context.deadline.remaining() );
        const auto relevant_kind = grab::wire_kind( spec.name );

        for( ;; )
        {
            checked = context.check();
            if( !checked )
            {
                return std::unexpected( std::move( checked.error() ) );
            }

            std::vector<grab::Event> pending;
            bool                     active{};
            int                      input_fd = invalidFd;
            {
                const std::scoped_lock lock{ state_mutex_ };
                input_fd = fd_;
                drain_available( input_fd, buffer_, active_, pending );
                active = active_;
            }

            bool relevant{};
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
                        relevant =
                            relevant || !relevant_kind || event.kind == *relevant_kind;
                        sink_copy( std::move( event ) );
                    }
                }
            }

            if( relevant )
            {
                return {};
            }
            if( !active )
            {
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                   "evdev device closed" );
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

            pollfd poll_target{
                .fd      = input_fd,
                .events  = static_cast<decltype( pollfd::events )>( POLLIN ),
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
                ::poll( &poll_target, pollTargetCount, timeout_milliseconds );
            if( ready < noError && errno == EINTR )
            {
                continue;
            }
            if( ready < noError )
            {
                return std::unexpected( posix_device_error( "poll evdev device",
                                                            errno ) );
            }
        }
    }

}    // namespace grab::drivers::device::evdev
