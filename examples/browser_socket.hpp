// browser_socket.hpp — a unix-socket listener that hands each accepted
// native-messaging connection to a grab BrowserBridge on a session's
// reactor/bus. Shared by the browser-driven examples.
//
// Wiring: register a native-messaging host in your browser whose executable
// forwards stdio to this socket, e.g.
//   #!/bin/sh
//   exec socat STDIO UNIX-CONNECT:"$XDG_RUNTIME_DIR/<name>.sock"
// The manifest (Chrome: ~/.config/google-chrome/NativeMessagingHosts/<x>.json,
// Firefox: ~/.mozilla/native-messaging-hosts/<x>.json) points "path" at that
// script; the grab webextension then streams tab events here.
#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "drivers/semantic/webextension/browser_bridge.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "kernel/scheduling/reactor.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <sys/epoll.h>    // NOLINT(misc-include-cleaner): provides EPOLLIN.
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace grab::examples
{

    inline constexpr int           listenBacklog   = 4;
    inline constexpr std::uint32_t epollInEvents   = EPOLLIN;
    inline constexpr int           socketInvalidFd = -1;

    // Default socket path under $XDG_RUNTIME_DIR (or /tmp), named per example.
    [[nodiscard]]
    inline std::string
    default_socket_path( std::string_view filename )
    {
        // NOLINTNEXTLINE(concurrency-mt-unsafe): read once before threads spawn.
        const char*       runtime_dir = std::getenv( "XDG_RUNTIME_DIR" );
        const std::string base =
            runtime_dir != nullptr ? std::string{ runtime_dir } : std::string{ "/tmp" };
        return base + "/" + std::string{ filename };
    }

    // Accepts native-messaging connections and hands each to a BrowserBridge
    // on the session's reactor/bus. Connection state is touched only on the
    // reactor thread; stop() serializes through a posted fence.
    class BrowserSocket
    {
        public:

            BrowserSocket()                       = default;
            ~BrowserSocket()                      = default;

            BrowserSocket( const BrowserSocket& ) = delete;
            BrowserSocket&
            operator=( const BrowserSocket& ) = delete;

            BrowserSocket( BrowserSocket&& other ) noexcept :
                path_{ std::move( other.path_ ) },
                listen_fd_{
                    std::exchange( other.listen_fd_,
                                   socketInvalidFd ),
                },
                token_{
                    std::exchange( other.token_,
                                   0U ),
                },
                state_{ std::move( other.state_ ) }
            {
            }

            BrowserSocket&
            operator=( BrowserSocket&& ) = delete;

            [[nodiscard]]
            static grab::Result<BrowserSocket>
            open( std::string    path,
                  grab::Session& session )
            {
                sockaddr_un address{};
                if( path.size() >= sizeof( address.sun_path ) )
                {
                    return std::unexpected( grab::Error{
                        .code       = grab::ErrorCode::InvalidArgument,
                        .message    = "socket path too long: " + path,
                        .capability = {},
                        .target     = {},
                        .attempts   = {},
                    } );
                }
                const int fd =
                    ::socket( AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0 );
                if( fd == socketInvalidFd )
                {
                    return std::unexpected( grab::Error{
                        .code = grab::ErrorCode::InternalFault,
                        .message =
                            "socket() failed: " +
                            // NOLINTNEXTLINE(concurrency-mt-unsafe): diagnostic only.
                            std::string{ std::strerror( errno ) },
                        .capability = {},
                        .target     = {},
                        .attempts   = {},
                    } );
                }
                address.sun_family = AF_UNIX;
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
                std::strncpy( address.sun_path,
                              path.c_str(),
                              sizeof( address.sun_path ) - 1U );
                ::unlink( path.c_str() );    // stale socket from a prior run
                if( ::bind(
                        fd,
                        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                        reinterpret_cast<const sockaddr*>( &address ),
                        sizeof( address )
                    ) !=
                    0 ||
                    ::listen( fd, listenBacklog ) != 0 )
                {
                    const int saved = errno;
                    ::close( fd );
                    return std::unexpected( grab::Error{
                        .code = grab::ErrorCode::InternalFault,
                        .message =
                            "bind/listen failed on " +
                            path +
                            ": " +
                            // NOLINTNEXTLINE(concurrency-mt-unsafe): diagnostic only.
                            std::string{ std::strerror( saved ) },
                        .capability = {},
                        .target     = {},
                        .attempts   = {},
                    } );
                }

                BrowserSocket socket;
                socket.path_        = std::move( path );
                socket.listen_fd_   = fd;
                socket.state_       = std::make_shared<State>();
                auto* const state   = socket.state_.get();
                auto&       reactor = session.reactor();
                auto&       bus     = session.bus();
                socket.token_ =
                    reactor.add_fd( fd,
                                    epollInEvents,
                                    [fd, state, &reactor, &bus]( std::uint32_t )
                                    {
                                        accept_pending( fd, *state, reactor, bus );
                                    } );
                return socket;
            }

            void
            stop( grab::Session& session )
            {
                if( listen_fd_ == socketInvalidFd )
                {
                    return;
                }
                session.reactor().remove_fd( token_ );
                // Serialize with any in-flight accept callback.
                std::promise<void> fence;
                auto               reached = fence.get_future();
                auto               posted  = session.post(
                    [this, &fence]
                    {
                        for( auto& connection : state_->connections )
                        {
                            connection.bridge.stop();
                            ::close( connection.fd );
                        }
                        state_->connections.clear();
                        fence.set_value();
                    }
                );
                if( posted.has_value() )
                {
                    reached.get();
                }
                ::close( listen_fd_ );
                ::unlink( path_.c_str() );
                listen_fd_ = socketInvalidFd;
            }

        private:

            struct Connection
            {
                    int                                                  fd;
                    grab::drivers::semantic::webextension::BrowserBridge bridge;
            };

            struct State
            {
                    std::vector<Connection> connections;    // reactor thread only
            };

            static void
            accept_pending( int                  listen_fd,
                            State&               state,
                            grab::core::Reactor& reactor,
                            grab::EventBus&      bus )
            {
                for( ;; )
                {
                    const int fd = ::accept4( listen_fd,
                                              nullptr,
                                              nullptr,
                                              SOCK_NONBLOCK | SOCK_CLOEXEC );
                    if( fd == socketInvalidFd )
                    {
                        return;    // EAGAIN or transient error: wait for next EPOLLIN
                    }
                    auto bridge =
                        grab::drivers::semantic::webextension::BrowserBridge::start(
                            fd,
                            reactor,
                            bus
                        );
                    if( !bridge.has_value() )
                    {
                        ::close( fd );
                        continue;
                    }
                    state.connections.push_back(
                        Connection{ .fd = fd, .bridge = std::move( *bridge ) }
                    );
                }
            }

            std::string   path_{};    // NOLINT(readability-redundant-member-init)
            int           listen_fd_ = socketInvalidFd;
            std::uint64_t token_     = 0U;
            std::shared_ptr<State>
                state_{};    // NOLINT(readability-redundant-member-init)
    };

}    // namespace grab::examples
