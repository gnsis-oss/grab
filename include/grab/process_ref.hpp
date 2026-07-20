#pragma once

#include "grab/result.hpp"

#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#ifndef __linux__
    #error "grab process ownership requires Linux 5.4 or newer"
#endif

#if !defined( __GLIBC__ ) || !defined( __GLIBC_MINOR__ )
    #error "grab process ownership requires glibc 2.36 or newer"
#else
// NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
static_assert( __GLIBC__ > 2 || ( __GLIBC__ == 2 && __GLIBC_MINOR__ >= 36 ),
               "grab process ownership requires glibc 2.36 or newer" );
#endif

namespace grab
{

    // Process ownership uses pidfd_open, pidfd_send_signal, and
    // waitid(P_PIDFD). It requires Linux 5.4 or newer and glibc 2.36 or newer.
    struct BorrowedProcessId
    {
            std::int64_t value{ -1 };

            friend auto
            operator<=>( const BorrowedProcessId&,
                         const BorrowedProcessId& ) = default;
    };

    struct ProcessSpawnOptions
    {
            // Search PATH for argv[0].
            bool search_path{ true };
    };

    // The only public process handle that can signal. It owns a pidfd and
    // retains the process start identity so numeric PID reuse is rejected.
    class OwnedProcess
    {
        public:

            OwnedProcess( OwnedProcess&& other ) noexcept;
            OwnedProcess&
            operator=( OwnedProcess&& other ) noexcept;
            OwnedProcess( const OwnedProcess& ) = delete;
            OwnedProcess&
            operator=( const OwnedProcess& ) = delete;
            ~OwnedProcess();

            // Spawn a child with CLONE_PIDFD so its pidfd is acquired
            // atomically, before any wait. An empty environment span inherits
            // the current process environment.
            [[nodiscard]]
            static Result<OwnedProcess>
            spawn( std::span<const std::string_view> argv,
                   std::span<const std::string_view> environment = {},
                   ProcessSpawnOptions               options     = {} );

            // Adopt only a direct child that has not exited or been reaped.
            // Arbitrary observed process IDs cannot be converted to ownership.
            [[nodiscard]]
            static Result<OwnedProcess>
            adopt_child( std::int64_t pid );

            [[nodiscard]]
            BorrowedProcessId
            id() const;

            [[nodiscard]]
            int
            pidfd() const noexcept
            {
                return pidfd_;
            }

            [[nodiscard]]
            bool
            alive() const;

            // Reaps one exit status. A zero timeout is a non-blocking probe.
            [[nodiscard]]
            Result<int>
            wait( std::chrono::milliseconds timeout );

            [[nodiscard]]
            Result<void>
            terminate( std::chrono::nanoseconds grace );

        private:

            OwnedProcess( int           pidfd,
                          std::int64_t  pid,
                          std::uint64_t start_token ) noexcept;

            int                pidfd_{ -1 };
            std::int64_t       pid_{ -1 };
            std::uint64_t      start_token_{};
            // terminate() preserves its reaped status for one later wait().
            std::optional<int> pending_wait_status_;
            bool               reaped_{};
    };

}    // namespace grab
