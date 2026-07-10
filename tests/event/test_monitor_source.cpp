#include "core/reactor.hpp"
#include "event/browser_bridge.hpp"
#include "event/monitor_source.hpp"
#include "event/source.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/pid.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
// clang-format on

namespace
{

    constexpr int              invalidFd            = -1;
    constexpr int              posixFailure         = -1;
    constexpr int              posixSuccess         = 0;
    constexpr int              pipeReadIndex        = 0;
    constexpr int              pipeWriteIndex       = 1;
    constexpr std::size_t      pipeFdCount          = 2U;
    constexpr ssize_t          noBytesWritten       = 0;
    constexpr std::size_t      frameHeaderBytes     = 4U;
    constexpr unsigned int     lengthByteOneShift   = 8U;
    constexpr unsigned int     lengthByteTwoShift   = 16U;
    constexpr unsigned int     lengthByteThreeShift = 24U;
    constexpr std::uint32_t    byteMask             = 0XFFU;
    constexpr std::size_t      subscriptionDepth    = 32U;
    constexpr auto             threadReadyTimeout   = std::chrono::seconds{ 2 };
    constexpr auto             registrationTimeout  = std::chrono::seconds{ 2 };
    constexpr auto             eventTimeout         = std::chrono::seconds{ 2 };
    constexpr auto             pollInterval         = std::chrono::milliseconds{ 10 };
    constexpr std::string_view sourceName           = "browser";
    constexpr std::string_view tabSwitchedJson =
        R"({"type":"browser.tab_switched","tab_title":"Gmail","prev_tab_title":"Docs","app":"chrome","pid":"42"})";
    constexpr std::string_view expectedTabTitle  = "Gmail";
    constexpr std::string_view expectedPrevTitle = "Docs";
    constexpr std::int64_t     expectedPidValue  = 42;
    constexpr grab::Pid        expectedPid{ expectedPidValue };
    constexpr std::string_view reactorDidNotStart  = "reactor thread did not start";
    constexpr std::string_view bridgeNotRegistered = "bridge fd did not register";

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
            read_fd() const noexcept
            {
                return read_fd_.get();
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
    std::optional<grab::Event>
    wait_for_event( grab::Subscription& subscription,
                    grab::EventKind     kind )
    {
        const auto deadline = std::chrono::steady_clock::now() + eventTimeout;
        while( std::chrono::steady_clock::now() < deadline )
        {
            while( auto event = subscription.try_pop() )
            {
                if( event->kind == kind )
                {
                    return event;
                }
            }
            std::this_thread::sleep_for( pollInterval );
        }
        return std::nullopt;
    }

    [[nodiscard]]
    std::string
    frame_message( std::string_view body )
    {
        const auto length = static_cast<std::uint32_t>( body.size() );
        std::array<char, frameHeaderBytes> header{
            static_cast<char>( length & byteMask ),
            static_cast<char>( ( length >> lengthByteOneShift ) & byteMask ),
            static_cast<char>( ( length >> lengthByteTwoShift ) & byteMask ),
            static_cast<char>( ( length >> lengthByteThreeShift ) & byteMask ),
        };

        std::string frame;
        frame.reserve( header.size() + body.size() );
        frame.append( header.data(), header.size() );
        frame.append( body.data(), body.size() );
        return frame;
    }

    [[nodiscard]]
    bool
    write_all( int              fd,
               std::string_view bytes )
    {
        std::size_t written = 0U;
        while( written < bytes.size() )
        {
            const auto remaining = bytes.substr( written );
            const auto result    = ::write( fd, remaining.data(), remaining.size() );
            if( result == posixFailure || result == noBytesWritten )
            {
                return false;
            }
            written += static_cast<std::size_t>( result );
        }
        return true;
    }

}    // namespace

TEST( MonitorSource,
      BrowserBridgeStartsPublishesAndStops )
{
    const Pipe pipe;
    ASSERT_TRUE( pipe.valid() );

    RunningReactor running;
    ASSERT_TRUE( running.wait_until_started() ) << reactorDidNotStart;

    grab::EventBus bus;
    auto           subscription = bus.subscribe(
        grab::EventFilter{
            .kinds      = { grab::EventKind::BrowserTabSwitched },
            .categories = {},
        },
        subscriptionDepth
    );
    const std::array kinds{ grab::EventKind::BrowserTabSwitched };

    grab::event::MonitorSource<grab::event::BrowserBridge> source(
        sourceName,
        std::span<const grab::EventKind>{ kinds },
        [&pipe]( grab::core::Reactor& reactor, grab::EventBus& event_bus )
        {
            return grab::event::BrowserBridge::start( pipe.read_fd(),
                                                      reactor,
                                                      event_bus );
        }
    );

    auto start_result = source.start( running.reactor(), bus );
    ASSERT_TRUE( start_result.has_value() ) << start_result.error().message;
    EXPECT_EQ( source.state(), grab::event::SourceState::Running );
    ASSERT_TRUE( wait_for_reactor_barrier( running.reactor() ) ) << bridgeNotRegistered;

    const auto frame = frame_message( tabSwitchedJson );
    ASSERT_TRUE( write_all( pipe.write_fd(), frame ) );

    auto event = wait_for_event( subscription, grab::EventKind::BrowserTabSwitched );
    ASSERT_TRUE( event.has_value() );
    const auto* payload = std::get_if<grab::BrowserTab>( &event->payload );
    ASSERT_NE( payload, nullptr );
    EXPECT_EQ( payload->pid, expectedPid );
    EXPECT_EQ( payload->tab_title, expectedTabTitle );
    EXPECT_EQ( payload->prev_tab_title, expectedPrevTitle );

    source.stop();
    EXPECT_EQ( source.state(), grab::event::SourceState::Stopped );

    running.stop_and_join();
    EXPECT_TRUE( running.result().has_value() );
}
