#include "drivers/device/evdev/evdev_source.hpp"
#include "grab/context.hpp"
#include "grab/event.hpp"
#include "grab/result.hpp"
#include "spi/event_source.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <mutex>
#include <span>
#include <string_view>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace grab::drivers::device::evdev
{
    namespace
    {

        constexpr const char*   missingDevicePath  = "/dev/input/does-not-exist-b6";
        constexpr int           invalidFd          = -1;
        constexpr int           posixFailure       = -1;
        constexpr int           posixSuccess       = 0;
        constexpr int           pipeReadIndex      = 0;
        constexpr int           pipeWriteIndex     = 1;
        constexpr std::size_t   pipeFdCount        = 2U;
        constexpr ssize_t       noBytesWritten     = 0;
        constexpr std::size_t   noEventCount       = 0U;
        constexpr std::size_t   singleRecordCount  = 1U;
        constexpr std::size_t   twoEventCount      = 2U;
        constexpr std::size_t   partialRecordBytes = sizeof( input_event ) / 2U;
        constexpr std::uint16_t evKeyType        = static_cast<std::uint16_t>( EV_KEY );
        constexpr std::uint16_t evRelType        = static_cast<std::uint16_t>( EV_REL );
        constexpr std::uint16_t keyACode         = static_cast<std::uint16_t>( KEY_A );
        constexpr std::uint16_t relXCode         = static_cast<std::uint16_t>( REL_X );
        constexpr std::int32_t  keyPressValue    = 1;
        constexpr std::int32_t  keyReleaseValue  = 0;
        constexpr std::int32_t  relXDelta        = 5;
        constexpr std::uint32_t expectedKeyACode = static_cast<std::uint32_t>( KEY_A );
        constexpr std::string_view expectedXAxis = "x";
        constexpr std::int64_t     inputEventSeconds  = 1;
        constexpr std::int64_t     inputEventUseconds = 2;
        constexpr auto             eventWaitBudget    = std::chrono::milliseconds{ 500 };
        constexpr auto             shortWaitBudget    = std::chrono::milliseconds{ 30 };

        class UniqueFd
        {
            public:

                explicit UniqueFd( int fd = invalidFd ) noexcept :
                    fd_( fd )
                {
                }

                ~UniqueFd() noexcept
                {
                    reset();
                }

                UniqueFd( const UniqueFd& ) = delete;
                UniqueFd&
                operator=( const UniqueFd& ) = delete;

                UniqueFd( UniqueFd&& other ) noexcept :
                    fd_( std::exchange( other.fd_,
                                        invalidFd ) )
                {
                }

                UniqueFd&
                operator=( UniqueFd&& other ) noexcept
                {
                    if( this != &other )
                    {
                        reset();
                        fd_ = std::exchange( other.fd_, invalidFd );
                    }
                    return *this;
                }

                [[nodiscard]]
                int
                get() const noexcept
                {
                    return fd_;
                }

                [[nodiscard]]
                int
                release() noexcept
                {
                    return std::exchange( fd_, invalidFd );
                }

            private:

                void
                reset() noexcept
                {
                    if( fd_ != invalidFd )
                    {
                        const auto close_result = ::close( fd_ );
                        static_cast<void>( close_result );
                        fd_ = invalidFd;
                    }
                }

                int fd_ = invalidFd;
        };

        class Pipe
        {
            public:

                Pipe()
                {
                    std::array<int, pipeFdCount> fds{};
                    valid_ = ::pipe( fds.data() ) == posixSuccess;
                    if( valid_ )
                    {
                        read_fd_  = UniqueFd{ fds.at( pipeReadIndex ) };
                        write_fd_ = UniqueFd{ fds.at( pipeWriteIndex ) };
                    }
                }

                ~Pipe()             = default;
                Pipe( const Pipe& ) = delete;
                Pipe&
                operator=( const Pipe& ) = delete;
                Pipe( Pipe&& )           = delete;
                Pipe&
                operator=( Pipe&& ) = delete;

                [[nodiscard]]
                bool
                valid() const noexcept
                {
                    return valid_;
                }

                [[nodiscard]]
                int
                release_read_fd() noexcept
                {
                    return read_fd_.release();
                }

                [[nodiscard]]
                int
                write_fd() const noexcept
                {
                    return write_fd_.get();
                }

            private:

                bool     valid_ = false;
                UniqueFd read_fd_;
                UniqueFd write_fd_;
        };

        class EventCollector
        {
            public:

                [[nodiscard]]
                std::function<void( grab::Event&& )>
                sink()
                {
                    return [this]( grab::Event&& event )
                    {
                        const std::scoped_lock lock{ mutex_ };
                        events_.push_back( std::move( event ) );
                    };
                }

                [[nodiscard]]
                std::vector<grab::Event>
                snapshot() const
                {
                    const std::scoped_lock lock{ mutex_ };
                    return events_;
                }

            private:

                mutable std::mutex       mutex_;
                std::vector<grab::Event> events_;
        };

        [[nodiscard]]
        input_event
        make_input_event( std::uint16_t type,
                          std::uint16_t code,
                          std::int32_t  value ) noexcept
        {
            input_event record{};
            record.input_event_sec =
                static_cast<decltype( record.input_event_sec )>( inputEventSeconds );
            record.input_event_usec =
                static_cast<decltype( record.input_event_usec )>( inputEventUseconds );
            record.type  = static_cast<decltype( record.type )>( type );
            record.code  = static_cast<decltype( record.code )>( code );
            record.value = static_cast<decltype( record.value )>( value );
            return record;
        }

        [[nodiscard]]
        std::span<const std::byte>
        input_event_bytes( const input_event& record ) noexcept
        {
            return std::as_bytes(
                std::span<const input_event>{ &record, singleRecordCount }
            );
        }

        [[nodiscard]]
        bool
        write_all( int                        fd,
                   std::span<const std::byte> bytes )
        {
            std::size_t written = 0U;
            while( written < bytes.size() )
            {
                const auto remaining = bytes.subspan( written );
                const auto result    = ::write( fd, remaining.data(), remaining.size() );
                if( result == posixFailure )
                {
                    if( errno == EINTR )
                    {
                        continue;
                    }
                    return false;
                }

                if( result == noBytesWritten )
                {
                    return false;
                }

                written += static_cast<std::size_t>( result );
            }
            return true;
        }

        [[nodiscard]]
        bool
        write_input_event( int                fd,
                           const input_event& record )
        {
            return write_all( fd, input_event_bytes( record ) );
        }

        [[nodiscard]]
        grab::Result<void>
        wait_for_any_event( EvdevSource&             source,
                            std::chrono::nanoseconds maximum_wait )
        {
            const grab::OperationContext context{
                .deadline = grab::Deadline::after( eventWaitBudget ),
            };
            const grab::spi::EventSpec spec{};
            return source.wait_for_event( spec, context, maximum_wait );
        }

    }    // namespace

    TEST( EvdevSource,
          DecodesKeyDownAndUp )
    {
        Pipe pipe;
        ASSERT_TRUE( pipe.valid() );

        EventCollector collector;
        auto           source_result = EvdevSource::adopt_fd( pipe.release_read_fd() );
        ASSERT_TRUE( source_result.has_value() ) << source_result.error().message;
        auto source = std::move( *source_result );
        source->set_sink( collector.sink() );

        const auto key_down = make_input_event( evKeyType, keyACode, keyPressValue );
        const auto key_up   = make_input_event( evKeyType, keyACode, keyReleaseValue );
        ASSERT_TRUE( write_input_event( pipe.write_fd(), key_down ) );
        ASSERT_TRUE( write_input_event( pipe.write_fd(), key_up ) );

        auto waited = wait_for_any_event( *source, eventWaitBudget );
        ASSERT_TRUE( waited.has_value() ) << waited.error().message;

        const auto events = collector.snapshot();
        ASSERT_EQ( events.size(), twoEventCount );
        EXPECT_EQ( events.at( 0U ).kind, grab::EventKind::KeyDown );
        const auto* first_payload =
            std::get_if<grab::InputKey>( &events.at( 0U ).payload );
        ASSERT_NE( first_payload, nullptr );
        EXPECT_EQ( first_payload->code, expectedKeyACode );

        EXPECT_EQ( events.at( 1U ).kind, grab::EventKind::KeyUp );
        const auto* second_payload =
            std::get_if<grab::InputKey>( &events.at( 1U ).payload );
        ASSERT_NE( second_payload, nullptr );
        EXPECT_EQ( second_payload->code, expectedKeyACode );
    }

    TEST( EvdevSource,
          DecodesRelativeMotion )
    {
        Pipe pipe;
        ASSERT_TRUE( pipe.valid() );

        EventCollector collector;
        auto           source_result = EvdevSource::adopt_fd( pipe.release_read_fd() );
        ASSERT_TRUE( source_result.has_value() ) << source_result.error().message;
        auto source = std::move( *source_result );
        source->set_sink( collector.sink() );

        const auto motion = make_input_event( evRelType, relXCode, relXDelta );
        ASSERT_TRUE( write_input_event( pipe.write_fd(), motion ) );

        auto waited = wait_for_any_event( *source, eventWaitBudget );
        ASSERT_TRUE( waited.has_value() ) << waited.error().message;

        const auto events = collector.snapshot();
        ASSERT_EQ( events.size(), singleRecordCount );
        EXPECT_EQ( events.front().kind, grab::EventKind::MouseMove );
        const auto* payload = std::get_if<grab::MouseMove>( &events.front().payload );
        ASSERT_NE( payload, nullptr );
        EXPECT_EQ( payload->axis, expectedXAxis );
        EXPECT_DOUBLE_EQ( payload->delta, static_cast<double>( relXDelta ) );
    }

    TEST( EvdevSource,
          HandlesPartialRecordAcrossReads )
    {
        Pipe pipe;
        ASSERT_TRUE( pipe.valid() );

        EventCollector collector;
        auto           source_result = EvdevSource::adopt_fd( pipe.release_read_fd() );
        ASSERT_TRUE( source_result.has_value() ) << source_result.error().message;
        auto source = std::move( *source_result );
        source->set_sink( collector.sink() );

        const auto record = make_input_event( evKeyType, keyACode, keyPressValue );
        const auto bytes  = input_event_bytes( record );
        ASSERT_TRUE( write_all( pipe.write_fd(), bytes.first( partialRecordBytes ) ) );

        auto waited = wait_for_any_event( *source, shortWaitBudget );
        ASSERT_TRUE( waited.has_value() ) << waited.error().message;
        EXPECT_EQ( collector.snapshot().size(), noEventCount );

        ASSERT_TRUE( write_all( pipe.write_fd(), bytes.subspan( partialRecordBytes ) ) );
        waited = wait_for_any_event( *source, eventWaitBudget );
        ASSERT_TRUE( waited.has_value() ) << waited.error().message;

        const auto events = collector.snapshot();
        ASSERT_EQ( events.size(), singleRecordCount );
        EXPECT_EQ( events.front().kind, grab::EventKind::KeyDown );
        const auto* payload = std::get_if<grab::InputKey>( &events.front().payload );
        ASSERT_NE( payload, nullptr );
        EXPECT_EQ( payload->code, expectedKeyACode );
    }

    TEST( EvdevSource,
          OpenBadDeviceFails )
    {
        auto source = EvdevSource::open_device( missingDevicePath );

        ASSERT_FALSE( source.has_value() );
        EXPECT_EQ( source.error().code, grab::ErrorCode::DeviceInaccessible );
    }

}    // namespace grab::drivers::device::evdev
