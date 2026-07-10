#include "core/reactor.hpp"
#include "event/browser_bridge.hpp"
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

    constexpr int              kInvalidFd            = -1;
    constexpr int              kPosixFailure         = -1;
    constexpr int              kPosixSuccess         = 0;
    constexpr int              kPipeReadIndex        = 0;
    constexpr int              kPipeWriteIndex       = 1;
    constexpr std::size_t      kPipeFdCount          = 2U;
    constexpr ssize_t          kNoBytesWritten       = 0;
    constexpr std::size_t      kFrameHeaderBytes     = 4U;
    constexpr unsigned int     kLengthByteOneShift   = 8U;
    constexpr unsigned int     kLengthByteTwoShift   = 16U;
    constexpr unsigned int     kLengthByteThreeShift = 24U;
    constexpr std::uint32_t    kByteMask             = 0XFFU;
    constexpr std::size_t      kSubscriptionDepth    = 32U;
    constexpr auto             kThreadReadyTimeout   = std::chrono::seconds{ 2 };
    constexpr auto             kRegistrationTimeout  = std::chrono::seconds{ 2 };
    constexpr auto             kEventTimeout         = std::chrono::seconds{ 2 };
    constexpr auto             kPollInterval         = std::chrono::milliseconds{ 10 };
    constexpr std::string_view kTabSwitchedJson =
        R"({"type":"browser.tab_switched","tab_title":"Gmail","prev_tab_title":"Docs","app":"chrome","pid":"42"})";
    constexpr std::string_view kSecondTabSwitchedJson =
        R"({"type":"browser.tab_switched","tab_title":"Calendar","prev_tab_title":"Gmail","app":"chrome","pid":"42"})";
    constexpr std::string_view kMalformedJson       = "{not json";
    constexpr std::string_view kMissingTypeJson     = R"({"tab_title":"x"})";
    constexpr std::string_view kExpectedTabTitle    = "Gmail";
    constexpr std::string_view kExpectedPrevTitle   = "Docs";
    constexpr std::string_view kSecondExpectedTitle = "Calendar";
    constexpr std::int64_t     kExpectedPidValue    = 42;
    constexpr grab::Pid        kExpectedPid{ kExpectedPidValue };
    constexpr std::string_view kReactorDidNotStart  = "reactor thread did not start";
    constexpr std::string_view kBridgeNotRegistered = "bridge fd did not register";

    class UniqueFd
    {
        public:

            explicit UniqueFd( int fd = kInvalidFd ) noexcept :
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
                                    kInvalidFd ) )
            {
            }

            UniqueFd&
            operator=( UniqueFd&& other ) noexcept
            {
                if( this != &other )
                {
                    reset();
                    fd_ = std::exchange( other.fd_, kInvalidFd );
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
                if( fd_ != kInvalidFd )
                {
                    const auto close_result = ::close( fd_ );
                    static_cast<void>( close_result );
                    fd_ = kInvalidFd;
                }
            }

            int fd_ = kInvalidFd;
    };

    class Pipe
    {
        public:

            Pipe()
            {
                std::array<int, kPipeFdCount> fds{};
                valid_ = ::pipe( fds.data() ) == kPosixSuccess;
                if( valid_ )
                {
                    read_fd_  = UniqueFd{ fds.at( kPipeReadIndex ) };
                    write_fd_ = UniqueFd{ fds.at( kPipeWriteIndex ) };
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
                return started_.wait_for( kThreadReadyTimeout ) ==
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
        return registered_future.wait_for( kRegistrationTimeout ) ==
               std::future_status::ready;
    }

    [[nodiscard]]
    std::optional<grab::Event>
    wait_for_event( grab::Subscription& subscription,
                    grab::EventKind     kind )
    {
        const auto deadline = std::chrono::steady_clock::now() + kEventTimeout;
        while( std::chrono::steady_clock::now() < deadline )
        {
            while( auto event = subscription.try_pop() )
            {
                if( event->kind == kind )
                {
                    return event;
                }
            }
            std::this_thread::sleep_for( kPollInterval );
        }
        return std::nullopt;
    }

    [[nodiscard]]
    std::string
    frame_message( std::string_view body )
    {
        const auto length = static_cast<std::uint32_t>( body.size() );
        std::array<char, kFrameHeaderBytes> header{
            static_cast<char>( length & kByteMask ),
            static_cast<char>( ( length >> kLengthByteOneShift ) & kByteMask ),
            static_cast<char>( ( length >> kLengthByteTwoShift ) & kByteMask ),
            static_cast<char>( ( length >> kLengthByteThreeShift ) & kByteMask ),
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
            if( result == kPosixFailure || result == kNoBytesWritten )
            {
                return false;
            }
            written += static_cast<std::size_t>( result );
        }
        return true;
    }

}    // namespace

TEST( BrowserBridge,
      ParsesTabSwitchedMessage )
{
    auto event = grab::event::parse_browser_message( kTabSwitchedJson );

    ASSERT_TRUE( event.has_value() ) << event.error().message;
    EXPECT_EQ( event->kind, grab::EventKind::browser_tab_switched );
    EXPECT_EQ( event->category, grab::EventCategory::browser );
    const auto* payload = std::get_if<grab::BrowserTab>( &event->payload );
    ASSERT_NE( payload, nullptr );
    EXPECT_EQ( payload->pid, kExpectedPid );
    EXPECT_EQ( payload->tab_title, kExpectedTabTitle );
    EXPECT_EQ( payload->prev_tab_title, kExpectedPrevTitle );
}

TEST( BrowserBridge,
      MalformedJsonRejected )
{
    auto event = grab::event::parse_browser_message( kMalformedJson );

    ASSERT_FALSE( event.has_value() );
    EXPECT_EQ( event.error().code, grab::ErrorCode::protocol_error );
}

TEST( BrowserBridge,
      MissingTypeRejected )
{
    auto event = grab::event::parse_browser_message( kMissingTypeJson );

    ASSERT_FALSE( event.has_value() );
    EXPECT_EQ( event.error().code, grab::ErrorCode::protocol_error );
}

TEST( BrowserBridge,
      BridgePublishesFramedMessage )
{
    const Pipe pipe;
    ASSERT_TRUE( pipe.valid() );

    RunningReactor running;
    ASSERT_TRUE( running.wait_until_started() ) << kReactorDidNotStart;

    grab::EventBus bus;
    auto           subscription = bus.subscribe(
        grab::EventFilter{
            .kinds      = { grab::EventKind::browser_tab_switched },
            .categories = {},
        },
        kSubscriptionDepth
    );

    auto bridge_result =
        grab::event::BrowserBridge::start( pipe.read_fd(), running.reactor(), bus );
    ASSERT_TRUE( bridge_result.has_value() ) << bridge_result.error().message;
    auto bridge = std::move( *bridge_result );
    ASSERT_TRUE( wait_for_reactor_barrier( running.reactor() ) ) << kBridgeNotRegistered;

    const auto frame = frame_message( kTabSwitchedJson );
    ASSERT_TRUE( write_all( pipe.write_fd(), frame ) );

    auto event = wait_for_event( subscription, grab::EventKind::browser_tab_switched );
    ASSERT_TRUE( event.has_value() );
    const auto* payload = std::get_if<grab::BrowserTab>( &event->payload );
    ASSERT_NE( payload, nullptr );
    EXPECT_EQ( payload->pid, kExpectedPid );
    EXPECT_EQ( payload->tab_title, kExpectedTabTitle );
    EXPECT_EQ( payload->prev_tab_title, kExpectedPrevTitle );

    bridge.stop();
    running.stop_and_join();
    EXPECT_TRUE( running.result().has_value() );
}

TEST( BrowserBridge,
      HandlesTwoFramesInOneWrite )
{
    const Pipe pipe;
    ASSERT_TRUE( pipe.valid() );

    RunningReactor running;
    ASSERT_TRUE( running.wait_until_started() ) << kReactorDidNotStart;

    grab::EventBus bus;
    auto           subscription = bus.subscribe(
        grab::EventFilter{
            .kinds      = { grab::EventKind::browser_tab_switched },
            .categories = {},
        },
        kSubscriptionDepth
    );

    auto bridge_result =
        grab::event::BrowserBridge::start( pipe.read_fd(), running.reactor(), bus );
    ASSERT_TRUE( bridge_result.has_value() ) << bridge_result.error().message;
    auto bridge = std::move( *bridge_result );
    ASSERT_TRUE( wait_for_reactor_barrier( running.reactor() ) ) << kBridgeNotRegistered;

    const auto frames =
        frame_message( kTabSwitchedJson ) + frame_message( kSecondTabSwitchedJson );
    ASSERT_TRUE( write_all( pipe.write_fd(), frames ) );

    auto first = wait_for_event( subscription, grab::EventKind::browser_tab_switched );
    ASSERT_TRUE( first.has_value() );
    auto second = wait_for_event( subscription, grab::EventKind::browser_tab_switched );
    ASSERT_TRUE( second.has_value() );

    const auto* first_payload = std::get_if<grab::BrowserTab>( &first->payload );
    ASSERT_NE( first_payload, nullptr );
    EXPECT_EQ( first_payload->tab_title, kExpectedTabTitle );

    const auto* second_payload = std::get_if<grab::BrowserTab>( &second->payload );
    ASSERT_NE( second_payload, nullptr );
    EXPECT_EQ( second_payload->tab_title, kSecondExpectedTitle );

    bridge.stop();
    running.stop_and_join();
    EXPECT_TRUE( running.result().has_value() );
}
