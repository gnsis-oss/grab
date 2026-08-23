#include "grab/process_ref.hpp"
#include "grab/result.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <expected>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <limits>
#include <linux/sched.h>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/poll.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C"
{
#include <sys/pidfd.h>
}

namespace grab
{
    namespace
    {

        constexpr int          posixFailure          = -1;
        constexpr unsigned int noPidfdFlags          = 0U;
        constexpr int          noWait                = 0;
        constexpr std::size_t  pipeReadEnd           = 0U;
        constexpr std::size_t  pipeWriteEnd          = 1U;
        constexpr int          execFailureExitStatus = 127;
        constexpr const char*  nullDevicePath        = "/dev/null";

        // Runs in the freshly cloned child, between clone3 and exec, so it
        // does only async-signal-safe work and reports nothing: a child that
        // cannot open /dev/null still execs, keeping the inherited streams,
        // which is strictly better than failing the spawn over the quietness
        // of a service.
        void
        redirect_standard_streams_to_null() noexcept
        {
            const int null_fd = ::open( nullDevicePath, O_RDWR | O_CLOEXEC );
            if( null_fd == posixFailure )
            {
                return;
            }
            for( const int stream : { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO } )
            {
                static_cast<void>( ::dup2( null_fd, stream ) );
            }
            if( null_fd > STDERR_FILENO )
            {
                static_cast<void>( ::close( null_fd ) );
            }
        }

        struct ProcessIdentity
        {
                std::int64_t  parent_pid{};
                std::uint64_t start_token{};
                char          state{};
        };

        enum class PidfdState : std::uint8_t
        {
            Running,
            Exited,
            Interrupted,
            Error,
        };

        class ScopedFd
        {
            public:

                explicit ScopedFd( int fd ) noexcept :
                    fd_( fd )
                {
                }

                ScopedFd( const ScopedFd& ) = delete;
                ScopedFd&
                operator=( const ScopedFd& ) = delete;
                ScopedFd( ScopedFd&& )       = delete;
                ScopedFd&
                operator=( ScopedFd&& ) = delete;

                ~ScopedFd()
                {
                    if( fd_ != posixFailure )
                    {
                        const int close_result = ::close( fd_ );
                        static_cast<void>( close_result );
                    }
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
                    return std::exchange( fd_, posixFailure );
                }

                void
                close() noexcept
                {
                    if( fd_ != posixFailure )
                    {
                        const int close_result = ::close( fd_ );
                        static_cast<void>( close_result );
                        fd_ = posixFailure;
                    }
                }

            private:

                int fd_;
        };

        [[nodiscard]]
        std::string
        posix_message( std::string_view operation,
                       int              error_number )
        {
            return std::string{ operation } +
                   ": " +
                   std::error_code{ error_number, std::generic_category() }.message();
        }

        [[nodiscard]]
        std::unexpected<Error>
        ownership_failure( std::string message )
        {
            return fail( ErrorCode::OwnershipRequired, std::move( message ) );
        }

        [[nodiscard]]
        std::expected<ProcessIdentity,
                      std::string>
        read_process_identity( std::int64_t pid )
        {
            const std::string path = "/proc/" + std::to_string( pid ) + "/stat";
            std::ifstream     input{ path };
            if( !input )
            {
                return std::unexpected( "cannot read process identity" );
            }

            const std::string contents{
                std::istreambuf_iterator<char>{ input },
                std::istreambuf_iterator<char>{}
            };
            const auto closing_parenthesis = contents.rfind( ')' );
            if( closing_parenthesis == std::string::npos )
            {
                return std::unexpected( "malformed process identity" );
            }

            std::istringstream fields{ contents.substr( closing_parenthesis + 1U ) };
            ProcessIdentity    identity;
            fields >> identity.state >> identity.parent_pid;

            constexpr int firstSkippedField = 5;
            constexpr int startTimeField    = 22;
            std::string   ignored;
            for( int field = firstSkippedField; field < startTimeField; ++field )
            {
                fields >> ignored;
            }
            fields >> identity.start_token;

            if( !fields )
            {
                return std::unexpected( "malformed process identity" );
            }
            return identity;
        }

        [[nodiscard]]
        bool
        is_dead_state( char state ) noexcept
        {
            constexpr char zombieState  = 'Z';
            constexpr char deadState    = 'X';
            constexpr char deadStateAlt = 'x';
            return state == zombieState || state == deadState || state == deadStateAlt;
        }

        [[nodiscard]]
        PidfdState
        poll_pidfd( int pidfd,
                    int timeout_milliseconds ) noexcept
        {
            pollfd descriptor{
                .fd      = pidfd,
                .events  = POLLIN,
                .revents = 0,
            };

            const int poll_result = ::poll( &descriptor, 1U, timeout_milliseconds );

            if( poll_result == posixFailure )
            {
                if( errno == EINTR )
                {
                    return PidfdState::Interrupted;
                }
                return PidfdState::Error;
            }
            if( poll_result == noWait )
            {
                return PidfdState::Running;
            }
            if( ( descriptor.revents & ( POLLNVAL | POLLERR ) ) != 0 )
            {
                return PidfdState::Error;
            }
            if( ( descriptor.revents & ( POLLIN | POLLHUP ) ) != 0 )
            {
                return PidfdState::Exited;
            }
            return PidfdState::Error;
        }

        [[nodiscard]]
        PidfdState
        poll_pidfd_now( int pidfd ) noexcept
        {
            auto state = PidfdState::Interrupted;
            while( state == PidfdState::Interrupted )
            {
                state = poll_pidfd( pidfd, noWait );
            }
            return state;
        }

        // NOLINTBEGIN(misc-include-cleaner): these POSIX types and constants are
        // provided through the documented platform headers above.
        [[nodiscard]]
        Result<int>
        reap_pidfd( int pidfd )
        {
            siginfo_t process_info{};
            int       wait_result = posixFailure;
            while( wait_result == posixFailure )
            {
                wait_result = ::waitid( P_PIDFD,
                                        static_cast<id_t>( pidfd ),
                                        &process_info,
                                        WEXITED );
                if( wait_result == posixFailure && errno != EINTR )
                {
                    break;
                }
            }

            if( wait_result == posixFailure )
            {
                if( errno == ECHILD )
                {
                    return ownership_failure( "process has already been reaped" );
                }
                return fail( ErrorCode::ProviderFailed,
                             posix_message( "waitid(P_PIDFD)", errno ) );
            }
            return process_info.si_status;
        }

        void
        reap_pidfd_best_effort( int pidfd ) noexcept
        {
            siginfo_t process_info{};
            int       wait_result = posixFailure;
            while( wait_result == posixFailure )
            {
                wait_result = ::waitid( P_PIDFD,
                                        static_cast<id_t>( pidfd ),
                                        &process_info,
                                        WEXITED );
                if( wait_result == posixFailure && errno != EINTR )
                {
                    break;
                }
            }
            static_cast<void>( wait_result );
        }

        // NOLINTEND(misc-include-cleaner)

        void
        clean_spawn_with_pidfd( int pidfd ) noexcept
        {
            const int signal_result =
                ::pidfd_send_signal( pidfd, SIGKILL, nullptr, noPidfdFlags );
            if( signal_result == 0 || errno == ESRCH )
            {
                reap_pidfd_best_effort( pidfd );
            }
        }

        [[nodiscard]]
        bool
        is_direct_child( const ProcessIdentity& identity ) noexcept
        {
            return identity.parent_pid == static_cast<std::int64_t>( ::getpid() );
        }

        [[nodiscard]]
        bool
        identity_matches( std::int64_t  pid,
                          std::uint64_t start_token )
        {
            const auto current = read_process_identity( pid );
            return current.has_value() &&
                   is_direct_child( *current ) &&
                   current->start_token == start_token;
        }

        [[nodiscard]]
        std::vector<std::string>
        copy_strings( std::span<const std::string_view> values )
        {
            std::vector<std::string> copies;
            copies.reserve( values.size() );
            for( const std::string_view value : values )
            {
                copies.emplace_back( value );
            }
            return copies;
        }

        [[nodiscard]]
        std::vector<char*>
        mutable_pointers( std::vector<std::string>& values )
        {
            std::vector<char*> pointers;
            pointers.reserve( values.size() + 1U );
            for( std::string& value : values )
            {
                pointers.push_back( value.data() );
            }
            pointers.push_back( nullptr );
            return pointers;
        }

        [[nodiscard]]
        bool
        contains_embedded_null( std::span<const std::string_view> values ) noexcept
        {
            return std::ranges::any_of( values,
                                        []( std::string_view value )
                                        {
                                            return value.contains( '\0' );
                                        } );
        }

        [[nodiscard]]
        int
        deadline_timeout( std::chrono::steady_clock::time_point deadline ) noexcept
        {
            const auto now = std::chrono::steady_clock::now();
            if( now >= deadline )
            {
                return noWait;
            }

            const auto remaining =
                std::chrono::ceil<std::chrono::milliseconds>( deadline - now );
            const auto maximum =
                std::chrono::milliseconds{ std::numeric_limits<int>::max() };
            return static_cast<int>( std::min( remaining, maximum ).count() );
        }

        [[nodiscard]]
        PidfdState
        wait_until( int                                   pidfd,
                    std::chrono::steady_clock::time_point deadline ) noexcept
        {
            while( true )
            {
                const int  timeout = deadline_timeout( deadline );
                const auto state   = poll_pidfd( pidfd, timeout );
                if( state == PidfdState::Interrupted )
                {
                    if( std::chrono::steady_clock::now() >= deadline )
                    {
                        return PidfdState::Running;
                    }
                    continue;
                }
                if( state !=
                    PidfdState::Running ||
                    std::chrono::steady_clock::now() >= deadline )
                {
                    return state;
                }
            }
        }

    }    // namespace

    OwnedProcess::OwnedProcess( int           pidfd,
                                std::int64_t  pid,
                                std::uint64_t start_token ) noexcept :
        pidfd_( pidfd ),
        pid_( pid ),
        start_token_( start_token )
    {
    }

    OwnedProcess::OwnedProcess( OwnedProcess&& other ) noexcept :
        pidfd_( std::exchange( other.pidfd_,
                               posixFailure ) ),
        pid_( std::exchange( other.pid_,
                             -1 ) ),
        start_token_( std::exchange( other.start_token_,
                                     0U ) ),
        pending_wait_status_( std::exchange( other.pending_wait_status_,
                                             std::nullopt ) ),
        reaped_( std::exchange( other.reaped_,
                                false ) )
    {
    }

    OwnedProcess&
    OwnedProcess::operator=( OwnedProcess&& other ) noexcept
    {
        if( this != &other )
        {
            if( pidfd_ != posixFailure )
            {
                const int close_result = ::close( pidfd_ );
                static_cast<void>( close_result );
            }
            pidfd_       = std::exchange( other.pidfd_, posixFailure );
            pid_         = std::exchange( other.pid_, -1 );
            start_token_ = std::exchange( other.start_token_, 0U );
            pending_wait_status_ =
                std::exchange( other.pending_wait_status_, std::nullopt );
            reaped_ = std::exchange( other.reaped_, false );
        }
        return *this;
    }

    OwnedProcess::~OwnedProcess()
    {
        if( pidfd_ != posixFailure )
        {
            const int close_result = ::close( pidfd_ );
            static_cast<void>( close_result );
        }
    }

    Result<OwnedProcess>
    OwnedProcess::spawn( std::span<const std::string_view> argv,
                         std::span<const std::string_view> environment,
                         ProcessSpawnOptions               options )
    {
        if( argv.empty() || argv.front().empty() )
        {
            return fail( ErrorCode::InvalidArgument,
                         "process argv must contain a non-empty executable" );
        }
        if( contains_embedded_null( argv ) || contains_embedded_null( environment ) )
        {
            return fail( ErrorCode::InvalidArgument,
                         "process arguments and environment cannot contain NUL" );
        }

        auto         argument_storage     = copy_strings( argv );
        auto         argument_pointers    = mutable_pointers( argument_storage );
        auto         environment_storage  = copy_strings( environment );
        auto         environment_pointers = mutable_pointers( environment_storage );
        char* const* environment_data =
            environment.empty() ? ::environ : environment_pointers.data();

        std::array<int, 2U> exec_pipe{};
        if( ::pipe2( exec_pipe.data(), O_CLOEXEC ) == posixFailure )
        {
            return fail( ErrorCode::ProviderFailed, posix_message( "pipe2", errno ) );
        }
        ScopedFd   exec_read_end{ exec_pipe[pipeReadEnd] };
        ScopedFd   exec_write_end{ exec_pipe[pipeWriteEnd] };

        int        raw_pidfd = posixFailure;
        clone_args clone_arguments{};
        clone_arguments.flags = static_cast<std::uint64_t>( CLONE_PIDFD );
        clone_arguments.pidfd =
            static_cast<std::uint64_t>( reinterpret_cast<std::uintptr_t>( &raw_pidfd ) );
        clone_arguments.exit_signal = static_cast<std::uint64_t>( SIGCHLD );

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): clone3 has no glibc
        // wrapper.
        const long clone_result =
            ::syscall( SYS_clone3, &clone_arguments, sizeof( clone_arguments ) );
        if( clone_result == posixFailure )
        {
            const int  error_number = errno;
            const auto code = error_number == ENOSYS ? ErrorCode::CapabilityUnavailable
                                                     : ErrorCode::ProviderFailed;
            return fail( code, posix_message( "clone3(CLONE_PIDFD)", error_number ) );
        }

        if( clone_result == 0 )
        {
            exec_read_end.close();
            if( options.discard_output )
            {
                redirect_standard_streams_to_null();
            }
            const int exec_result = options.search_path
                                      ? ::execvpe( argument_pointers.front(),
                                                   argument_pointers.data(),
                                                   environment_data )
                                      : ::execve( argument_pointers.front(),
                                                  argument_pointers.data(),
                                                  environment_data );
            static_cast<void>( exec_result );
            const int execution_error = errno;

            ssize_t   write_result    = posixFailure;
            while( write_result == posixFailure )
            {
                write_result = ::write( exec_write_end.get(),
                                        &execution_error,
                                        sizeof( execution_error ) );
                if( write_result == posixFailure && errno != EINTR )
                {
                    break;
                }
            }
            ::_exit( execFailureExitStatus );
        }

        if( raw_pidfd ==
            posixFailure ||
            clone_result > std::numeric_limits<pid_t>::max() )
        {
            return fail( ErrorCode::InternalFault,
                         "clone3 did not return a valid child receipt" );
        }

        const auto child_pid = static_cast<pid_t>( clone_result );
        ScopedFd   pidfd{ raw_pidfd };
        exec_write_end.close();

        int     execution_error{};
        ssize_t read_result = posixFailure;
        while( read_result == posixFailure )
        {
            read_result = ::read( exec_read_end.get(),
                                  &execution_error,
                                  sizeof( execution_error ) );
            if( read_result == posixFailure && errno != EINTR )
            {
                break;
            }
        }

        if( read_result != 0 )
        {
            if( read_result == static_cast<ssize_t>( sizeof( execution_error ) ) )
            {
                reap_pidfd_best_effort( pidfd.get() );
                return fail( ErrorCode::ProviderFailed,
                             posix_message( "exec", execution_error ) );
            }
            const int error_number = read_result == posixFailure ? errno : EIO;
            clean_spawn_with_pidfd( pidfd.get() );
            return fail( ErrorCode::ProviderFailed,
                         posix_message( "exec status pipe", error_number ) );
        }

        const auto first_identity  = read_process_identity( child_pid );
        const auto second_identity = read_process_identity( child_pid );
        const auto state           = poll_pidfd_now( pidfd.get() );
        if( !first_identity.has_value() ||
            !second_identity.has_value() ||
            !is_direct_child( *first_identity ) ||
            !is_direct_child( *second_identity ) ||
            first_identity->start_token !=
            second_identity->start_token ||
            ( state != PidfdState::Running && state != PidfdState::Exited ) )
        {
            if( state == PidfdState::Exited )
            {
                reap_pidfd_best_effort( pidfd.get() );
            }
            else
            {
                clean_spawn_with_pidfd( pidfd.get() );
            }
            return ownership_failure( "spawned child does not have a stable identity" );
        }

        return OwnedProcess{
            pidfd.release(),
            static_cast<std::int64_t>( child_pid ),
            first_identity->start_token
        };
    }

    Result<OwnedProcess>
    OwnedProcess::adopt_child( std::int64_t pid )
    {
        if( pid <=
            0 ||
            pid > static_cast<std::int64_t>( std::numeric_limits<pid_t>::max() ) )
        {
            return ownership_failure( "invalid child process id" );
        }
        const auto native_pid     = static_cast<pid_t>( pid );

        const auto first_identity = read_process_identity( pid );
        if( !first_identity.has_value() || !is_direct_child( *first_identity ) )
        {
            return ownership_failure( "process is not a live direct child" );
        }

        const int raw_pidfd = ::pidfd_open( native_pid, noPidfdFlags );
        if( raw_pidfd == posixFailure )
        {
            return ownership_failure( "cannot acquire child process ownership" );
        }
        ScopedFd   pidfd{ raw_pidfd };

        const auto second_identity = read_process_identity( pid );
        if( !second_identity.has_value() ||
            !is_direct_child( *second_identity ) ||
            first_identity->start_token != second_identity->start_token )
        {
            return ownership_failure( "child process identity changed" );
        }

        const auto state = poll_pidfd_now( pidfd.get() );
        if( is_dead_state( first_identity->state ) ||
            is_dead_state( second_identity->state ) ||
            state != PidfdState::Running )
        {
            if( state == PidfdState::Exited )
            {
                reap_pidfd_best_effort( pidfd.get() );
            }
            return ownership_failure( "process is not a live direct child" );
        }

        return OwnedProcess{ pidfd.release(), pid, first_identity->start_token };
    }

    BorrowedProcessId
    OwnedProcess::id() const
    {
        return BorrowedProcessId{ .value = pid_ };
    }

    bool
    OwnedProcess::alive() const
    {
        return pidfd_ != posixFailure && poll_pidfd_now( pidfd_ ) == PidfdState::Running;
    }

    Result<int>
    OwnedProcess::wait( std::chrono::milliseconds timeout )
    {
        if( pidfd_ == posixFailure )
        {
            return ownership_failure( "process handle is empty" );
        }
        if( pending_wait_status_.has_value() )
        {
            const int status = *pending_wait_status_;
            pending_wait_status_.reset();
            return status;
        }
        if( reaped_ )
        {
            return ownership_failure( "process has already been reaped" );
        }

        const auto now = std::chrono::steady_clock::now();
        const auto maximum_timeout =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::time_point::max() - now
            );
        const auto bounded_timeout =
            std::clamp( timeout, std::chrono::milliseconds::zero(), maximum_timeout );
        const auto state = wait_until( pidfd_, now + bounded_timeout );
        if( state == PidfdState::Running )
        {
            return fail( ErrorCode::DeadlineExceeded, "process wait timed out" );
        }
        if( state == PidfdState::Error )
        {
            return fail( ErrorCode::ProviderFailed, "poll(pidfd) failed" );
        }

        auto status = reap_pidfd( pidfd_ );
        if( !status.has_value() )
        {
            return std::unexpected( std::move( status.error() ) );
        }
        reaped_ = true;
        return *status;
    }

    Result<void>
    // NOLINTNEXTLINE(readability-make-member-function-const): signals and reaps the
    // owned child.
    OwnedProcess::terminate( std::chrono::nanoseconds grace )
    {
        if( pidfd_ == posixFailure || !identity_matches( pid_, start_token_ ) )
        {
            return ownership_failure( "process identity no longer matches" );
        }

        const auto reap_and_cache_status = [this]() -> Result<void>
        {
            auto status = reap_pidfd( pidfd_ );
            if( !status.has_value() )
            {
                return std::unexpected( std::move( status.error() ) );
            }
            pending_wait_status_ = *status;
            reaped_              = true;
            return {};
        };

        const int term_result =
            ::pidfd_send_signal( pidfd_, SIGTERM, nullptr, noPidfdFlags );
        if( term_result == posixFailure )
        {
            if( errno == ESRCH && poll_pidfd_now( pidfd_ ) == PidfdState::Exited )
            {
                return reap_and_cache_status();
            }
            return fail( ErrorCode::ProviderFailed,
                         posix_message( "pidfd_send_signal(SIGTERM)", errno ) );
        }

        const auto now           = std::chrono::steady_clock::now();
        const auto maximum_grace = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::time_point::max() - now
        );
        const auto bounded_grace =
            std::clamp( grace, std::chrono::nanoseconds::zero(), maximum_grace );
        const auto deadline = now + bounded_grace;
        const auto state    = wait_until( pidfd_, deadline );
        if( state == PidfdState::Exited )
        {
            return reap_and_cache_status();
        }
        if( state == PidfdState::Error )
        {
            return fail( ErrorCode::ProviderFailed, "poll(pidfd) failed" );
        }

        if( !identity_matches( pid_, start_token_ ) )
        {
            if( poll_pidfd_now( pidfd_ ) == PidfdState::Exited )
            {
                return reap_and_cache_status();
            }
            return ownership_failure( "process identity no longer matches" );
        }

        const int kill_result =
            ::pidfd_send_signal( pidfd_, SIGKILL, nullptr, noPidfdFlags );
        if( kill_result == posixFailure && errno != ESRCH )
        {
            return fail( ErrorCode::ProviderFailed,
                         posix_message( "pidfd_send_signal(SIGKILL)", errno ) );
        }
        return reap_and_cache_status();
    }

}    // namespace grab
