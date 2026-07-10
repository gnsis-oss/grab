#include "core/reactor.hpp"
#include "event/evdev.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <optional>
#include <span>
#include <string_view>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
// clang-format on

namespace
{

    constexpr const char*      missingDevicePath  = "/dev/input/does-not-exist-b6";
    constexpr int              invalidFd          = -1;
    constexpr int              posixFailure       = -1;
    constexpr int              posixSuccess       = 0;
    constexpr int              pipeReadIndex      = 0;
    constexpr int              pipeWriteIndex     = 1;
    constexpr std::size_t      pipeFdCount        = 2U;
    constexpr ssize_t          noBytesWritten     = 0;
    constexpr std::size_t      singleRecordCount  = 1U;
    constexpr std::size_t      partialRecordBytes = sizeof( input_event ) / 2U;
    constexpr std::uint16_t    evKeyType          = static_cast<std::uint16_t>( EV_KEY );
    constexpr std::uint16_t    evRelType          = static_cast<std::uint16_t>( EV_REL );
    constexpr std::uint16_t    keyACode           = static_cast<std::uint16_t>( KEY_A );
    constexpr std::uint16_t    relXCode           = static_cast<std::uint16_t>( REL_X );
    constexpr std::int32_t     keyPressValue      = 1;
    constexpr std::int32_t     keyReleaseValue    = 0;
    constexpr std::int32_t     relXDelta          = 5;
    constexpr std::uint32_t    expectedKeyACode   = static_cast<std::uint32_t>( KEY_A );
    constexpr std::string_view expectedXAxis      = "x";
    constexpr std::size_t      subscriptionDepth  = 32U;
    constexpr std::int64_t     inputEventSeconds  = 1;
    constexpr std::int64_t     inputEventUseconds = 2;
    constexpr auto             threadReadyTimeout = std::chrono::seconds{ 2 };
    constexpr auto             registrationTimeout   = std::chrono::seconds{ 2 };
    constexpr auto             eventTimeout          = std::chrono::seconds{ 2 };
    constexpr auto             pollInterval          = std::chrono::milliseconds{ 10 };
    constexpr auto             noEventWindow         = std::chrono::milliseconds{ 30 };
    constexpr std::string_view reactorDidNotStart    = "reactor thread did not start";
    constexpr std::string_view monitorDidNotRegister = "evdev fd did not register";

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

    class RunningReactor
    {
        public:

            RunningReactor() :
                started_( reactor_started_.get_future() ),
                thread_(
                    [this]
                    {
                        reactor_started_.set_value();
                        result_ = reactor_.run();
                    }
                )
            {
            }

            ~RunningReactor()
            {
                stop_and_join();
            }

            RunningReactor( const RunningReactor& ) = delete;
            RunningReactor&
            operator=( const RunningReactor& ) = delete;
            RunningReactor( RunningReactor&& ) = delete;
            RunningReactor&
            operator=( RunningReactor&& ) = delete;

            [[nodiscard]]
            bool
            wait_until_started()
            {
                return started_.wait_for( threadReadyTimeout ) ==
                       std::future_status::ready;
            }

            [[nodiscard]]
            grab::core::Reactor&
            reactor() noexcept
            {
                return reactor_;
            }

            void
            stop_and_join() noexcept
            {
                reactor_.stop();
                if( thread_.joinable() )
                {
                    thread_.join();
                }
            }

            [[nodiscard]]
            const grab::Result<void>&
            result() const noexcept
            {
                return result_;
            }

        private:

            grab::core::Reactor reactor_;
            std::promise<void>  reactor_started_;
            std::future<void>   started_;
            grab::Result<void>  result_;
            std::thread         thread_;
    };

    [[nodiscard]]
    bool
    wait_for_reactor_barrier( grab::core::Reactor& reactor )
    {
        std::promise<void> registered;
        auto               registered_future = registered.get_future();
        reactor.post(
            [&registered]
            {
                registered.set_value();
            }
        );
        return registered_future.wait_for( registrationTimeout ) ==
               std::future_status::ready;
    }

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
    std::optional<grab::Event>
    wait_for_next_event( grab::Subscription&                 subscription,
                         std::chrono::steady_clock::duration timeout )
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while( std::chrono::steady_clock::now() < deadline )
        {
            if( auto event = subscription.try_pop() )
            {
                return event;
            }
            std::this_thread::sleep_for( pollInterval );
        }
        return std::nullopt;
    }

    [[nodiscard]]
    std::optional<grab::Event>
    wait_for_next_event( grab::Subscription& subscription )
    {
        return wait_for_next_event( subscription, eventTimeout );
    }

    [[nodiscard]]
    grab::EventFilter
    input_filter()
    {
        grab::EventFilter filter;
        filter.kinds = {
            grab::EventKind::KeyDown,
            grab::EventKind::KeyUp,
            grab::EventKind::MouseMove,
        };
        return filter;
    }

}    // namespace

TEST( Evdev,
      DecodesKeyDownAndUp )
{
    Pipe pipe;
    ASSERT_TRUE( pipe.valid() );

    RunningReactor running;
    ASSERT_TRUE( running.wait_until_started() ) << reactorDidNotStart;

    grab::EventBus bus;
    auto           subscription = bus.subscribe( input_filter(), subscriptionDepth );

    auto monitor_result = grab::event::EvdevMonitor::adopt_fd( pipe.release_read_fd(),
                                                               running.reactor(),
                                                               bus );
    ASSERT_TRUE( monitor_result.has_value() ) << monitor_result.error().message;
    auto monitor = std::move( *monitor_result );
    ASSERT_TRUE( wait_for_reactor_barrier( running.reactor() ) )
        << monitorDidNotRegister;

    const auto key_down = make_input_event( evKeyType, keyACode, keyPressValue );
    const auto key_up   = make_input_event( evKeyType, keyACode, keyReleaseValue );
    ASSERT_TRUE( write_input_event( pipe.write_fd(), key_down ) );
    ASSERT_TRUE( write_input_event( pipe.write_fd(), key_up ) );

    auto first = wait_for_next_event( subscription );
    ASSERT_TRUE( first.has_value() );
    EXPECT_EQ( first->kind, grab::EventKind::KeyDown );
    const auto* first_payload = std::get_if<grab::InputKey>( &first->payload );
    ASSERT_NE( first_payload, nullptr );
    EXPECT_EQ( first_payload->code, expectedKeyACode );

    auto second = wait_for_next_event( subscription );
    ASSERT_TRUE( second.has_value() );
    EXPECT_EQ( second->kind, grab::EventKind::KeyUp );
    const auto* second_payload = std::get_if<grab::InputKey>( &second->payload );
    ASSERT_NE( second_payload, nullptr );
    EXPECT_EQ( second_payload->code, expectedKeyACode );

    monitor.stop();
    running.stop_and_join();
    EXPECT_TRUE( running.result().has_value() );
}

TEST( Evdev,
      DecodesRelativeMotion )
{
    Pipe pipe;
    ASSERT_TRUE( pipe.valid() );

    RunningReactor running;
    ASSERT_TRUE( running.wait_until_started() ) << reactorDidNotStart;

    grab::EventBus bus;
    auto           subscription = bus.subscribe( input_filter(), subscriptionDepth );

    auto monitor_result = grab::event::EvdevMonitor::adopt_fd( pipe.release_read_fd(),
                                                               running.reactor(),
                                                               bus );
    ASSERT_TRUE( monitor_result.has_value() ) << monitor_result.error().message;
    auto monitor = std::move( *monitor_result );
    ASSERT_TRUE( wait_for_reactor_barrier( running.reactor() ) )
        << monitorDidNotRegister;

    const auto motion = make_input_event( evRelType, relXCode, relXDelta );
    ASSERT_TRUE( write_input_event( pipe.write_fd(), motion ) );

    auto event = wait_for_next_event( subscription );
    ASSERT_TRUE( event.has_value() );
    EXPECT_EQ( event->kind, grab::EventKind::MouseMove );
    const auto* payload = std::get_if<grab::MouseMove>( &event->payload );
    ASSERT_NE( payload, nullptr );
    EXPECT_EQ( payload->axis, expectedXAxis );
    EXPECT_DOUBLE_EQ( payload->delta, static_cast<double>( relXDelta ) );

    monitor.stop();
    running.stop_and_join();
    EXPECT_TRUE( running.result().has_value() );
}

TEST( Evdev,
      HandlesPartialRecordAcrossReads )
{
    Pipe pipe;
    ASSERT_TRUE( pipe.valid() );

    RunningReactor running;
    ASSERT_TRUE( running.wait_until_started() ) << reactorDidNotStart;

    grab::EventBus bus;
    auto           subscription = bus.subscribe( input_filter(), subscriptionDepth );

    auto monitor_result = grab::event::EvdevMonitor::adopt_fd( pipe.release_read_fd(),
                                                               running.reactor(),
                                                               bus );
    ASSERT_TRUE( monitor_result.has_value() ) << monitor_result.error().message;
    auto monitor = std::move( *monitor_result );
    ASSERT_TRUE( wait_for_reactor_barrier( running.reactor() ) )
        << monitorDidNotRegister;

    const auto record = make_input_event( evKeyType, keyACode, keyPressValue );
    const auto bytes  = input_event_bytes( record );
    ASSERT_TRUE( write_all( pipe.write_fd(), bytes.first( partialRecordBytes ) ) );
    std::this_thread::sleep_for( noEventWindow );
    EXPECT_FALSE( subscription.try_pop().has_value() );

    ASSERT_TRUE( write_all( pipe.write_fd(), bytes.subspan( partialRecordBytes ) ) );

    auto event = wait_for_next_event( subscription );
    ASSERT_TRUE( event.has_value() );
    EXPECT_EQ( event->kind, grab::EventKind::KeyDown );
    const auto* payload = std::get_if<grab::InputKey>( &event->payload );
    ASSERT_NE( payload, nullptr );
    EXPECT_EQ( payload->code, expectedKeyACode );
    EXPECT_FALSE( wait_for_next_event( subscription, noEventWindow ).has_value() );

    monitor.stop();
    running.stop_and_join();
    EXPECT_TRUE( running.result().has_value() );
}

TEST( Evdev,
      OpenBadDeviceFails )
{
    grab::core::Reactor reactor;
    grab::EventBus      bus;

    auto                monitor =
        grab::event::EvdevMonitor::open_device( missingDevicePath, reactor, bus );

    ASSERT_FALSE( monitor.has_value() );
    EXPECT_EQ( monitor.error().code, grab::ErrorCode::DeviceInaccessible );
}
