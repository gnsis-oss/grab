#include "grab/capability.hpp"
#include "grab/process_ref.hpp"
#include "grab/provisioning.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "session/display_probe.hpp"
#include "session/poll_wait.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
// mkdtemp and setenv are POSIX; <cstdlib> is not required to declare them.
#include <stdlib.h>    // NOLINT(modernize-deprecated-headers,hicpp-deprecated-headers)
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace grab
{
    namespace
    {

        using EnvironmentOverrides = std::vector<std::pair<std::string, std::string>>;

        // A display server that has not accepted a connection within this is
        // not coming up. The number is generous because a cold X server on a
        // loaded machine is slow, and the wait ends early on readiness anyway.
        constexpr auto             displayReadyTimeout = std::chrono::seconds{ 10 };
        // A window manager or compositing manager announces itself by claiming
        // a selection, which it does as one of its first acts.
        constexpr auto             serviceReadyTimeout = std::chrono::seconds{ 5 };
        constexpr auto             busReadyTimeout     = std::chrono::seconds{ 10 };
        // SIGTERM, then SIGKILL. Teardown must terminate even for a child that
        // declines to exit — a browser mid-shutdown outliving the display it
        // was drawing on is the common case, not the exotic one.
        constexpr auto             terminationGrace = std::chrono::seconds{ 2 };
        // Auto-picked display numbers are contended: two callers scanning at
        // once see the same free number, and the loser's server exits at once.
        constexpr int              displayAttempts    = 8;
        constexpr int              socketFailure      = -1;
        constexpr int              callFailure        = -1;
        constexpr int              overwriteVariable  = 1;

        constexpr std::string_view colorDepth         = "24";
        constexpr std::string_view displayVariable    = "DISPLAY";
        constexpr std::string_view busVariable        = "DBUS_SESSION_BUS_ADDRESS";
        constexpr std::string_view unixAddressPrefix  = "unix:path=";
        constexpr std::string_view runtimeDirTemplate = "grab-display-XXXXXX";
        constexpr std::string_view busSocketName      = "bus";

        // The bridge switches an AT-SPI-reading application needs before it
        // exposes a tree at all. A consumer cannot be expected to know this
        // list; handing it back rather than documenting it is the point.
        constexpr std::array<std::pair<std::string_view, std::string_view>, 4>
            accessibilityBridgeVariables{
                std::pair{
                          std::string_view{ "GNOME_ACCESSIBILITY" },
                          std::string_view{ "1" }                                          },
                std::pair{
                          std::string_view{ "GTK_MODULES" },
                          std::string_view{ "gail:atk-bridge" }                            },
                std::pair{
                          std::string_view{ "QT_ACCESSIBILITY" },
                          std::string_view{ "1" }                                          },
                std::pair{      std::string_view{ "NO_AT_BRIDGE" }, std::string_view{ "0" }},
        };

        struct ServiceCandidate
        {
                std::string_view              executable;
                std::vector<std::string_view> arguments;
        };

        // Which window manager and which compositing manager are
        // implementation details: a consumer should not have to name openbox
        // or xcompmgr, and naming one would tie grab's preconditions to one
        // package. Order is preference; the first that is installed AND claims
        // its selection wins.
        [[nodiscard]]
        std::vector<ServiceCandidate>
        window_manager_candidates()
        {
            return {
                {                .executable = "openbox", .arguments = {}},
                {                .executable = "fluxbox", .arguments = {}},
                {                  .executable = "marco", .arguments = {}},
                {                  .executable = "xfwm4", .arguments = {}},
                {               .executable = "metacity", .arguments = {}},
                {                    .executable = "jwm", .arguments = {}},
                {.executable = "matchbox-window-manager", .arguments = {}},
                {                    .executable = "twm", .arguments = {}},
            };
        }

        [[nodiscard]]
        std::vector<ServiceCandidate>
        compositor_candidates()
        {
            return {
                // --config /dev/null so the operator's own picom settings
                // cannot change what grab's overlay is composited against.
                {
                 .executable = "picom",
                 .arguments =
                        {
                            "--backend",
                            "xrender",
                            "--config",
                            "/dev/null",
                        }, },
                { .executable = "xcompmgr", .arguments = {} },
                { .executable = "compton", .arguments = { "--backend", "xrender" } },
            };
        }

        [[nodiscard]]
        std::string
        candidate_names( const std::vector<ServiceCandidate>& candidates )
        {
            std::string names;
            for( const auto& candidate : candidates )
            {
                if( !names.empty() )
                {
                    names += ", ";
                }
                names += candidate.executable;
            }
            return names;
        }

        [[nodiscard]]
        std::optional<std::string>
        environment_value( std::string_view name )
        {
            const std::string key{ name };
            // NOLINTNEXTLINE(concurrency-mt-unsafe): a read of the environment.
            const char* const value = std::getenv( key.c_str() );
            if( value == nullptr || *value == '\0' )
            {
                return std::nullopt;
            }
            return std::string{ value };
        }

        [[nodiscard]]
        bool
        executable_at( const std::string& path )
        {
            return !path.empty() && access( path.c_str(), X_OK ) == 0;
        }

        // Resolve on PATH here rather than letting exec search, so a missing
        // service reads as "not installed" — a reason a caller can act on —
        // instead of as a failed spawn.
        [[nodiscard]]
        std::optional<std::string>
        find_executable( std::string_view name )
        {
            const std::string wanted{ name };
            if( wanted.contains( '/' ) )
            {
                return executable_at( wanted ) ? std::optional{ wanted } : std::nullopt;
            }

            const auto search_path = environment_value( "PATH" );
            if( !search_path.has_value() )
            {
                return std::nullopt;
            }

            std::string_view remaining{ *search_path };
            for( ;; )
            {
                const auto  separator = remaining.find( ':' );
                const auto  directory = remaining.substr( 0U, separator );
                std::string candidate{
                    directory.empty() ? std::string_view{ "." } : directory
                };
                candidate += '/';
                candidate += wanted;
                if( executable_at( candidate ) )
                {
                    return candidate;
                }
                if( separator == std::string_view::npos )
                {
                    return std::nullopt;
                }
                remaining.remove_prefix( separator + 1U );
            }
        }

        // The AT-SPI helpers are on no distribution's PATH: they are pieces a
        // desktop session activates, so they live in a libexec directory that
        // moves between distributions.
        [[nodiscard]]
        std::optional<std::string>
        find_accessibility_helper( std::string_view name )
        {
            if( auto on_path = find_executable( name ); on_path.has_value() )
            {
                return on_path;
            }
            constexpr std::array<std::string_view, 5> libexecDirectories{
                "/usr/libexec",
                "/usr/lib/at-spi2-core",
                "/usr/lib/x86_64-linux-gnu/at-spi2-core",
                "/usr/lib64/at-spi2-core",
                "/usr/lib/at-spi2",
            };
            for( const auto& directory : libexecDirectories )
            {
                std::string candidate{ directory };
                candidate += '/';
                candidate += name;
                if( executable_at( candidate ) )
                {
                    return candidate;
                }
            }
            return std::nullopt;
        }

        // A full environment block: this process's, with the overrides
        // applied. Passing only the overrides would strip the child of HOME,
        // PATH and XAUTHORITY, none of which grab has any business removing.
        [[nodiscard]]
        std::vector<std::string>
        environment_with( const EnvironmentOverrides& overrides )
        {
            std::vector<std::string> entries;
            // environ is a NULL-terminated array of pointers, and walking it
            // is the only way to read it.
            // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            for( char** cursor = environ; cursor != nullptr && *cursor != nullptr;
                 ++cursor )
            {
                const std::string entry{ *cursor };
                const std::string key = entry.substr( 0U, entry.find( '=' ) );
                const bool        overridden =
                    std::ranges::any_of( overrides,
                                         [&key]( const auto& replacement )
                                         {
                                             return replacement.first == key;
                                         } );
                if( !overridden )
                {
                    entries.push_back( entry );
                }
            }
            // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            for( const auto& [key, value] : overrides )
            {
                std::string entry{ key };
                entry += '=';
                entry += value;
                entries.push_back( std::move( entry ) );
            }
            return entries;
        }

        [[nodiscard]]
        grab::Result<grab::OwnedProcess>
        spawn_service( const std::string&              executable,
                       const std::vector<std::string>& arguments,
                       const EnvironmentOverrides&     overrides )
        {
            std::vector<std::string> argument_storage;
            argument_storage.reserve( arguments.size() + 1U );
            argument_storage.push_back( executable );
            argument_storage.insert( argument_storage.end(),
                                     arguments.begin(),
                                     arguments.end() );

            const std::vector<std::string> environment_storage =
                environment_with( overrides );

            std::vector<std::string_view> argv;
            argv.reserve( argument_storage.size() );
            for( const auto& argument : argument_storage )
            {
                argv.emplace_back( argument );
            }
            std::vector<std::string_view> environment;
            environment.reserve( environment_storage.size() );
            for( const auto& entry : environment_storage )
            {
                environment.emplace_back( entry );
            }

            return grab::OwnedProcess::spawn(
                std::span<const std::string_view>{ argv },
                std::span<const std::string_view>{ environment },
                grab::ProcessSpawnOptions{
                    // The path is already resolved, and a service's own
                    // chatter is not its caller's report.
                    .search_path    = false,
                    .discard_output = true,
                }
            );
        }

        [[nodiscard]]
        grab::Error
        precondition_error( std::string_view                   capability_id,
                            std::string                        reason,
                            std::string                        target,
                            std::vector<grab::ProviderAttempt> attempts = {} )
        {
            return grab::Error{
                .code       = grab::ErrorCode::CapabilityUnavailable,
                .message    = std::move( reason ),
                .capability = std::string{ capability_id },
                .target     = std::move( target ),
                .attempts   = std::move( attempts ),
            };
        }

        [[nodiscard]]
        bool
        unix_socket_connectable( const std::string& path )
        {
            struct stat information{};
            if( ::stat( path.c_str(), &information ) !=
                0 ||
                S_ISSOCK( information.st_mode ) == 0 )
            {
                return false;
            }

            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            if( path.size() >= sizeof( address.sun_path ) )
            {
                return false;
            }
            const int descriptor = ::socket( AF_UNIX, SOCK_STREAM, 0 );
            if( descriptor == socketFailure )
            {
                return false;
            }
            std::memcpy( static_cast<void*>( &address.sun_path[0] ),
                         path.data(),
                         path.size() );
            const bool connected =
                ::connect( descriptor,
                           // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                           reinterpret_cast<const sockaddr*>( &address ),
                           sizeof( address ) ) == 0;
            static_cast<void>( ::close( descriptor ) );
            return connected;
        }

        // Waits for a service to announce itself, and gives up early when the
        // service dies rather than burning the whole timeout on a corpse.
        [[nodiscard]]
        session::Probe
        await_service( grab::OwnedProcess&          process,
                       const std::function<bool()>& ready,
                       std::chrono::milliseconds    timeout )
        {
            return session::poll_until(
                [&process, &ready]() -> session::Probe
                {
                    if( ready() )
                    {
                        return session::Probe::Ready;
                    }
                    return process.alive() ? session::Probe::Retry
                                           : session::Probe::Abandoned;
                },
                timeout
            );
        }

        [[nodiscard]]
        std::string
        service_failure_reason( session::Probe            outcome,
                                const std::string&        announcement,
                                std::chrono::milliseconds timeout )
        {
            if( outcome == session::Probe::Abandoned )
            {
                return "exited before it " + announcement;
            }
            return "did not " +
                   announcement +
                   " within " +
                   std::to_string( timeout.count() ) +
                   "ms";
        }

        [[nodiscard]]
        constexpr std::chrono::milliseconds
        as_milliseconds( std::chrono::seconds seconds ) noexcept
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>( seconds );
        }

        [[nodiscard]]
        std::string
        screen_geometry( std::uint16_t width,
                         std::uint16_t height )
        {
            return std::to_string( width ) + "x" + std::to_string( height );
        }

        [[nodiscard]]
        std::vector<std::string>
        display_server_arguments( const DisplayRequest& request,
                                  const std::string&    display )
        {
            const std::string geometry =
                screen_geometry( request.width, request.height );
            std::vector<std::string> arguments{ display };
            if( request.backend == DisplayBackend::Nested )
            {
                arguments.insert( arguments.end(),
                                  {
                                      "-screen",
                                      geometry,
                                      "-title",
                                      "grab provisioned display " + display,
                                      "-resizeable",
                                  } );
            }
            else
            {
                arguments.insert( arguments.end(),
                                  {
                                      "-screen",
                                      "0",
                                      geometry + "x" + std::string{ colorDepth },
                                      // The NVIDIA EGL vendor library segfaults
                                      // Xvfb during GLX init on some hosts, and
                                      // nothing grab does needs GL.
                                      "-extension",
                                      "GLX",
                                  } );
            }
            // Without -noreset the readiness probe's disconnect — it is the
            // last client at that moment — resets the server and races every
            // later connect. COMPOSITE is what makes a compositing manager
            // possible at all, and grab's overlay needs one.
            arguments.insert( arguments.end(),
                              {
                                  "-noreset",
                                  "+extension",
                                  "COMPOSITE",
                                  "+extension",
                                  "RANDR",
                                  "+extension",
                                  "XTEST",
                              } );
            return arguments;
        }

        [[nodiscard]]
        grab::Result<void>
        validate( const DisplayRequest& request )
        {
            if( request.display.has_value() && request.display->empty() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "display name must not be empty" );
            }
            if( request.backend == DisplayBackend::Existing )
            {
                return {};
            }
            if( request.width == 0U || request.height == 0U )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "display width and height must both be non-zero" );
            }
            if( request.accessibility_bus && !request.session_bus )
            {
                return grab::fail(
                    grab::ErrorCode::InvalidArgument,
                    "accessibility_bus needs session_bus: the AT-SPI bus is "
                    "started on, and advertised through, a session bus"
                );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<std::string>
        existing_display_name( const DisplayRequest& request )
        {
            if( request.display.has_value() )
            {
                return *request.display;
            }
            if( auto ambient = environment_value( displayVariable );
                ambient.has_value() )
            {
                return *ambient;
            }
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "DisplayBackend::Existing needs a display: set "
                               "DisplayRequest::display or DISPLAY" );
        }

        // Only the nested backend needs an environment of its own: its window
        // opens on the operator's display, which is the one place this whole
        // API touches a display it did not create.
        [[nodiscard]]
        grab::Result<EnvironmentOverrides>
        display_server_environment( const DisplayRequest& request )
        {
            if( request.backend != DisplayBackend::Nested )
            {
                return EnvironmentOverrides{};
            }
            const auto host = request.host_display.has_value()
                                ? request.host_display
                                : environment_value( displayVariable );
            if( !host.has_value() || host->empty() )
            {
                return grab::fail(
                    grab::ErrorCode::InvalidArgument,
                    "DisplayBackend::Nested needs a host display to open its "
                    "window on: set DisplayRequest::host_display or DISPLAY"
                );
            }
            return EnvironmentOverrides{
                { std::string{ displayVariable }, *host },
            };
        }

        [[nodiscard]]
        grab::Result<std::filesystem::path>
        make_runtime_directory()
        {
            const std::string base =
                environment_value( "XDG_RUNTIME_DIR" ).value_or( std::string{ "/tmp" } );
            std::string pattern = base + "/" + std::string{ runtimeDirTemplate };
            if( ::mkdtemp( pattern.data() ) == nullptr )
            {
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                   "cannot create a private runtime directory under " +
                                       base );
            }
            return std::filesystem::path{ pattern };
        }

    }    // namespace

    // The provisioning state machine, and the owner of everything it started.
    class ProvisionedDisplay::Impl
    {
        public:

            // A service this object started, remembered by the only handle
            // that can safely signal it: OwnedProcess holds a pidfd and the
            // process start identity, so teardown cannot reach a recycled pid
            // and never has to match on a name.
            struct Service
            {
                    std::string        label;
                    grab::OwnedProcess process;
            };

            Impl( std::string display_name,
                  bool        accessibility_requested ) :
                display( std::move( display_name ) ),
                accessibility_expected( accessibility_requested )
            {
            }

            ~Impl()
            {
                teardown();
            }

            Impl( const Impl& ) = delete;
            Impl&
            operator=( const Impl& ) = delete;
            Impl( Impl&& )           = delete;
            Impl&
            operator=( Impl&& ) = delete;

            // Youngest first: the accessibility stack before the bus it talks
            // to, every client before the display it was drawing on, and the
            // display last.
            void
            teardown() noexcept
            {
                probe.reset();
                while( !services.empty() )
                {
                    auto& service = services.back();
                    if( !service.process.terminate( terminationGrace ).has_value() )
                    {
                        // The service had already exited on its own, and
                        // terminate() will not signal a process whose identity
                        // no longer matches. Reap it here rather than leave a
                        // zombie behind in a long-lived consumer.
                        auto reaped =
                            service.process.wait( std::chrono::milliseconds::zero() );
                        static_cast<void>( reaped );
                    }
                    services.pop_back();
                }
                if( !runtime_directory.empty() )
                {
                    std::error_code failure;
                    static_cast<void>( std::filesystem::remove_all( runtime_directory,
                                                                    failure ) );
                    runtime_directory.clear();
                }
            }

            [[nodiscard]]
            grab::Result<void>
            attach_probe()
            {
                auto opened = session::DisplayProbe::open( display );
                if( !opened.has_value() )
                {
                    return std::unexpected( std::move( opened.error() ) );
                }
                probe = std::move( *opened );
                return {};
            }

            [[nodiscard]]
            EnvironmentOverrides
            display_only() const
            {
                return EnvironmentOverrides{
                    { std::string{ displayVariable }, display },
                };
            }

            // Start the first candidate that is installed and actually claims
            // the role. One that starts but never claims it is stopped again:
            // leaving it running would be a process nobody owns, doing
            // nothing, on a display grab is about to report as unusable.
            [[nodiscard]]
            grab::Result<void>
            start_first_working( const std::vector<ServiceCandidate>& candidates,
                                 const std::function<bool()>&         ready,
                                 const std::string&                   announcement,
                                 std::string_view                     capability_id,
                                 const std::string&                   role )
            {
                std::vector<grab::ProviderAttempt> attempts;
                for( const auto& candidate : candidates )
                {
                    const auto executable = find_executable( candidate.executable );
                    if( !executable.has_value() )
                    {
                        attempts.push_back( grab::ProviderAttempt{
                            .provider = std::string{ candidate.executable },
                            .reason   = "not installed",
                        } );
                        continue;
                    }

                    std::vector<std::string> arguments;
                    arguments.reserve( candidate.arguments.size() );
                    for( const auto& argument : candidate.arguments )
                    {
                        arguments.emplace_back( argument );
                    }

                    auto process =
                        spawn_service( *executable, arguments, display_only() );
                    if( !process.has_value() )
                    {
                        attempts.push_back( grab::ProviderAttempt{
                            .provider = std::string{ candidate.executable },
                            .reason   = process.error().message,
                        } );
                        continue;
                    }

                    const auto outcome =
                        await_service( *process,
                                       ready,
                                       as_milliseconds( serviceReadyTimeout ) );
                    if( outcome == session::Probe::Ready )
                    {
                        services.push_back( Service{
                            .label   = std::string{ candidate.executable },
                            .process = std::move( *process ),
                        } );
                        return {};
                    }

                    auto stopped = process->terminate( terminationGrace );
                    static_cast<void>( stopped );
                    attempts.push_back( grab::ProviderAttempt{
                        .provider = std::string{ candidate.executable },
                        .reason   = service_failure_reason(
                            outcome,
                            announcement,
                            as_milliseconds( serviceReadyTimeout )
                        ),
                    } );
                }

                return std::unexpected(
                    precondition_error( capability_id,
                                        "no " +
                                            role +
                                            " could be started on " +
                                            display +
                                            "; install one of: " +
                                            candidate_names( candidates ),
                                        display,
                                        std::move( attempts ) )
                );
            }

            [[nodiscard]]
            grab::Result<void>
            start_display_server( const DisplayRequest&       request,
                                  const std::string&          display_name,
                                  const EnvironmentOverrides& server_environment )
            {
                const std::string_view name = request.backend == DisplayBackend::Nested
                                                ? std::string_view{ "Xephyr" }
                                                : std::string_view{ "Xvfb" };
                const auto             executable = find_executable( name );
                if( !executable.has_value() )
                {
                    return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                       std::string{ name } +
                                           " is not installed; it is what "
                                           "provides the display grab drives" );
                }

                auto process =
                    spawn_service( *executable,
                                   display_server_arguments( request, display_name ),
                                   server_environment );
                if( !process.has_value() )
                {
                    return std::unexpected( std::move( process.error() ) );
                }

                // Readiness is the display accepting a connection AND our own
                // child still running: a display number lost to a racing
                // caller is connectable too, and inheriting somebody else's
                // server would mean tearing it down later.
                const auto outcome = await_service(
                    *process,
                    [&display_name]()
                    {
                        return session::display_connectable( display_name );
                    },
                    as_milliseconds( displayReadyTimeout )
                );
                if( outcome != session::Probe::Ready )
                {
                    auto stopped = process->terminate( terminationGrace );
                    static_cast<void>( stopped );
                    return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                       std::string{ name } +
                                           " on " +
                                           display_name +
                                           " " +
                                           service_failure_reason(
                                               outcome,
                                               "accepted connections",
                                               as_milliseconds( displayReadyTimeout )
                                           ) );
                }

                display = display_name;
                services.push_back( Service{
                    .label   = std::string{ name },
                    .process = std::move( *process ),
                } );
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            start_session_bus()
            {
                auto directory = make_runtime_directory();
                if( !directory.has_value() )
                {
                    return std::unexpected( std::move( directory.error() ) );
                }
                runtime_directory = *directory;

                const std::string socket_path =
                    ( runtime_directory / std::string{ busSocketName } ).string();
                const std::string address =
                    std::string{ unixAddressPrefix } + socket_path;

                const auto executable = find_executable( "dbus-daemon" );
                if( !executable.has_value() )
                {
                    return std::unexpected( precondition_error(
                        {},
                        "dbus-daemon is not installed; the accessibility bus "
                        "grab reads the tree from needs a session bus to sit on",
                        display
                    ) );
                }

                // The address is chosen rather than read back: a socket path
                // we picked is observable with a plain connect(), where
                // --print-address would need a pipe and a parse.
                auto process =
                    spawn_service( *executable,
                                   { "--session", "--nofork", "--address=" + address },
                                   display_only() );
                if( !process.has_value() )
                {
                    return std::unexpected( std::move( process.error() ) );
                }

                const auto outcome = await_service(
                    *process,
                    [&socket_path]()
                    {
                        return unix_socket_connectable( socket_path );
                    },
                    as_milliseconds( busReadyTimeout )
                );
                if( outcome != session::Probe::Ready )
                {
                    auto stopped = process->terminate( terminationGrace );
                    static_cast<void>( stopped );
                    return std::unexpected( precondition_error(
                        {},
                        "dbus-daemon " +
                            service_failure_reason( outcome,
                                                    "accept connections on " +
                                                        socket_path,
                                                    as_milliseconds( busReadyTimeout ) ),
                        display
                    ) );
                }

                bus_address = address;
                services.push_back( Service{
                    .label   = "dbus-daemon",
                    .process = std::move( *process ),
                } );
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            start_accessibility_bus()
            {
                const auto launcher = find_accessibility_helper( "at-spi-bus-launcher" );
                if( !launcher.has_value() )
                {
                    return std::unexpected( precondition_error(
                        {},
                        "at-spi-bus-launcher was not found; install "
                        "at-spi2-core, which provides the accessibility bus "
                        "grab reads the tree from",
                        display
                    ) );
                }

                const EnvironmentOverrides overrides{
                    {std::string{ displayVariable },     display},
                    {    std::string{ busVariable }, bus_address},
                };

                auto process =
                    spawn_service( *launcher, { "--launch-immediately" }, overrides );
                if( !process.has_value() )
                {
                    return std::unexpected( std::move( process.error() ) );
                }

                // The launcher advertises the bus it started as AT_SPI_BUS on
                // the root window, which is how an AT-SPI client on X11 finds
                // it — so that property appearing IS the readiness signal.
                const auto outcome = await_service(
                    *process,
                    [this]()
                    {
                        return probe.has_value() &&
                               probe->accessibility_bus_address().has_value();
                    },
                    as_milliseconds( busReadyTimeout )
                );
                if( outcome != session::Probe::Ready )
                {
                    auto stopped = process->terminate( terminationGrace );
                    static_cast<void>( stopped );
                    return std::unexpected( precondition_error(
                        {},
                        "at-spi-bus-launcher " +
                            service_failure_reason( outcome,
                                                    "advertise AT_SPI_BUS on " + display,
                                                    as_milliseconds( busReadyTimeout ) ),
                        display
                    ) );
                }

                services.push_back( Service{
                    .label   = "at-spi-bus-launcher",
                    .process = std::move( *process ),
                } );

                // The registry is bus-activatable, so starting it here is an
                // optimisation rather than a requirement: it means the first
                // resolve() does not pay for the activation round trip.
                const auto registry = find_accessibility_helper( "at-spi2-registryd" );
                if( registry.has_value() )
                {
                    auto registry_process = spawn_service( *registry, {}, overrides );
                    if( registry_process.has_value() )
                    {
                        services.push_back( Service{
                            .label   = "at-spi2-registryd",
                            .process = std::move( *registry_process ),
                        } );
                    }
                }
                return {};
            }

            // Attach: nothing is started, reconfigured or torn down. A
            // second window manager or compositing manager on a desktop
            // somebody is using would be destructive, so the preconditions
            // are reported instead of imposed.
            [[nodiscard]]
            static grab::Result<std::unique_ptr<Impl>>
            attach( const DisplayRequest& request )
            {
                auto display = existing_display_name( request );
                if( !display.has_value() )
                {
                    return std::unexpected( std::move( display.error() ) );
                }
                auto state =
                    std::make_unique<Impl>( *display, request.accessibility_bus );
                auto attached = state->attach_probe();
                if( !attached.has_value() )
                {
                    return std::unexpected( std::move( attached.error() ) );
                }
                return state;
            }

            // Create: the display first, then the preconditions in the only
            // order that works — the display must accept connections before a
            // window manager can start on it, and the session bus must exist
            // before the accessibility bus that is advertised through it.
            // Anything already started is torn down by ~Impl on the way out of
            // a failure, so a half-provisioned display never escapes.
            [[nodiscard]]
            static grab::Result<std::unique_ptr<Impl>>
            create( const DisplayRequest& request )
            {
                auto server_environment = display_server_environment( request );
                if( !server_environment.has_value() )
                {
                    return std::unexpected( std::move( server_environment.error() ) );
                }

                auto state =
                    std::make_unique<Impl>( std::string{}, request.accessibility_bus );
                auto started =
                    state->start_requested_display( request, *server_environment );
                if( !started.has_value() )
                {
                    return std::unexpected( std::move( started.error() ) );
                }
                auto attached = state->attach_probe();
                if( !attached.has_value() )
                {
                    return std::unexpected( std::move( attached.error() ) );
                }

                if( request.window_manager )
                {
                    auto manager = state->start_first_working(
                        window_manager_candidates(),
                        [&state]()
                        {
                            return state->probe->window_manager_present();
                        },
                        "take the input focus",
                        capability::mouse_click,
                        "window manager"
                    );
                    if( !manager.has_value() )
                    {
                        return std::unexpected( std::move( manager.error() ) );
                    }
                }
                if( request.compositor )
                {
                    auto compositing = state->start_first_working(
                        compositor_candidates(),
                        [&state]()
                        {
                            return state->probe->compositor_present();
                        },
                        "claim the compositing manager selection",
                        capability::overlay,
                        "compositing manager"
                    );
                    if( !compositing.has_value() )
                    {
                        return std::unexpected( std::move( compositing.error() ) );
                    }
                }
                if( request.session_bus )
                {
                    auto bus = state->start_session_bus();
                    if( !bus.has_value() )
                    {
                        return std::unexpected( std::move( bus.error() ) );
                    }
                }
                if( request.accessibility_bus )
                {
                    auto accessibility = state->start_accessibility_bus();
                    if( !accessibility.has_value() )
                    {
                        return std::unexpected( std::move( accessibility.error() ) );
                    }
                }
                return state;
            }

            // A named display is taken at its word; an unnamed one is scanned
            // for, and losing the scan's race is retried rather than reported,
            // because the loser's server exits immediately and looks exactly
            // like a server that failed to start.
            [[nodiscard]]
            grab::Result<void>
            start_requested_display( const DisplayRequest&       request,
                                     const EnvironmentOverrides& server_environment )
            {
                if( request.display.has_value() )
                {
                    if( session::display_connectable( *request.display ) )
                    {
                        return grab::fail(
                            grab::ErrorCode::DeviceInaccessible,
                            "display " +
                                *request.display +
                                " is already in use; pick another, or use "
                                "DisplayBackend::Existing to attach to it"
                        );
                    }
                    return start_display_server( request,
                                                 *request.display,
                                                 server_environment );
                }

                grab::Error last_error;
                for( int attempt = 0; attempt < displayAttempts; ++attempt )
                {
                    auto free_display = session::find_free_display();
                    if( !free_display.has_value() )
                    {
                        return std::unexpected( std::move( free_display.error() ) );
                    }
                    auto started = start_display_server( request,
                                                         *free_display,
                                                         server_environment );
                    if( started.has_value() )
                    {
                        return {};
                    }
                    last_error = std::move( started.error() );
                }
                return std::unexpected( std::move( last_error ) );
            }

            std::string                          display;
            bool                                 accessibility_expected;
            std::string                          bus_address;
            std::filesystem::path                runtime_directory;
            std::vector<Service>                 services;
            std::optional<session::DisplayProbe> probe;
    };

    ProvisionedDisplay::ProvisionedDisplay( std::unique_ptr<Impl> impl ) noexcept :
        impl_( std::move( impl ) )
    {
    }

    ProvisionedDisplay::~ProvisionedDisplay()                               = default;

    ProvisionedDisplay::ProvisionedDisplay( ProvisionedDisplay&& ) noexcept = default;

    ProvisionedDisplay&
    ProvisionedDisplay::operator=( ProvisionedDisplay&& ) noexcept = default;

    std::string_view
    ProvisionedDisplay::name() const noexcept
    {
        return impl_ == nullptr ? std::string_view{}
                                : std::string_view{ impl_->display };
    }

    std::vector<std::pair<std::string,
                          std::string>>
    ProvisionedDisplay::child_environment() const
    {
        std::vector<std::pair<std::string, std::string>> environment;
        if( impl_ == nullptr )
        {
            return environment;
        }

        environment.emplace_back( std::string{ displayVariable }, impl_->display );

        // A display grab provisioned has a session bus of its own; a display
        // it attached to has whatever the operator's session is using, and
        // passing that through is what makes one call complete in both cases.
        const std::string bus =
            impl_->bus_address.empty()
                ? environment_value( busVariable ).value_or( std::string{} )
                : impl_->bus_address;
        if( !bus.empty() )
        {
            environment.emplace_back( std::string{ busVariable }, bus );
        }

        if( impl_->accessibility_expected )
        {
            for( const auto& [key, value] : accessibilityBridgeVariables )
            {
                environment.emplace_back( std::string{ key }, std::string{ value } );
            }
        }
        return environment;
    }

    Result<void>
    ProvisionedDisplay::adopt_environment() const
    {
        for( const auto& [key, value] : child_environment() )
        {
            // NOLINTNEXTLINE(concurrency-mt-unsafe): documented single-thread use.
            if( ::setenv( key.c_str(), value.c_str(), overwriteVariable ) ==
                callFailure )
            {
                return grab::fail( ErrorCode::InternalFault,
                                   "cannot set " + key + " in this process" );
            }
        }
        return {};
    }

    Result<void>
    ProvisionedDisplay::window_manager() const
    {
        if( impl_ == nullptr || !impl_->probe.has_value() )
        {
            return std::unexpected(
                precondition_error( capability::mouse_click,
                                    "the provisioned display has been moved "
                                    "from or torn down",
                                    std::string{ name() } )
            );
        }
        if( impl_->probe->window_manager_present() )
        {
            return {};
        }
        return std::unexpected( precondition_error(
            capability::mouse_click,
            "no window manager holds the input focus on " +
                impl_->display +
                " (neither _NET_SUPPORTING_WM_CHECK nor the ICCCM manager "
                "selection is claimed); a synthetic click is delivered and "
                "activates nothing",
            impl_->display
        ) );
    }

    Result<void>
    ProvisionedDisplay::compositor() const
    {
        if( impl_ == nullptr || !impl_->probe.has_value() )
        {
            return std::unexpected(
                precondition_error( capability::overlay,
                                    "the provisioned display has been moved "
                                    "from or torn down",
                                    std::string{ name() } )
            );
        }
        if( impl_->probe->compositor_present() )
        {
            return {};
        }
        // The same sentence the X11 overlay delegate reports when it refuses
        // to draw, so one condition reads as one condition wherever it
        // surfaces.
        return std::unexpected( precondition_error(
            capability::overlay,
            "X11 overlay requires an owned compositing manager selection (" +
                impl_->probe->compositor_selection_name() +
                " on " +
                impl_->display +
                " is unowned)",
            impl_->display
        ) );
    }

    Result<void>
    ProvisionedDisplay::accessibility() const
    {
        if( impl_ == nullptr || !impl_->probe.has_value() )
        {
            return std::unexpected(
                precondition_error( {},
                                    "the provisioned display has been moved "
                                    "from or torn down",
                                    std::string{ name() } )
            );
        }
        if( impl_->probe->accessibility_bus_address().has_value() )
        {
            return {};
        }
        return std::unexpected( precondition_error(
            {},
            "no accessibility bus is advertised on " +
                impl_->display +
                " (the AT_SPI_BUS root-window property is unset); resolve and "
                "describe have no tree to read",
            impl_->display
        ) );
    }

    Result<ProvisionedDisplay>
    provision_display( DisplayRequest request )
    {
        auto valid = validate( request );
        if( !valid.has_value() )
        {
            return std::unexpected( std::move( valid.error() ) );
        }

        auto state = request.backend == DisplayBackend::Existing
                       ? ProvisionedDisplay::Impl::attach( request )
                       : ProvisionedDisplay::Impl::create( request );
        if( !state.has_value() )
        {
            return std::unexpected( std::move( state.error() ) );
        }
        return ProvisionedDisplay{ std::move( *state ) };
    }

    Result<std::unique_ptr<Session>>
    open_session( const ProvisionedDisplay& display,
                  SessionOptions            options )
    {
        if( display.name().empty() )
        {
            return grab::fail( ErrorCode::InvalidArgument,
                               "the provisioned display has been moved from or "
                               "torn down; there is no display to open on" );
        }

        auto adopted = display.adopt_environment();
        if( !adopted.has_value() )
        {
            return std::unexpected( std::move( adopted.error() ) );
        }
        options.display = std::string{ display.name() };
        return Session::open( std::move( options ) );
    }

}    // namespace grab
