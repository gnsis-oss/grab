#pragma once

// ┌──────────────────────────────────────────────────────────────────────────┐
// │  host.hpp — bring up a private X session with a browser on it, and kill  │
// │  exactly what was started.                                               │
// │                                                                          │
// │  Extracted verbatim from view_demo.cpp, where it was file-local inside a │
// │  1772-line example and therefore unreachable by anything else. Every rung │
// │  of the ladder from `button` upward needs a browser; none of them should  │
// │  own a second copy of this.                                              │
// │                                                                          │
// │  Lives under examples/ rather than include/ on purpose. Process          │
// │  management is not part of the graduatable library — precedent is        │
// │  examples/smoke.hpp.                                                     │
// │                                                                          │
// │  The two services that are NOT optional, and why:                        │
// │    * a window manager — without one Firefox never takes input focus and  │
// │      a synthetic click is delivered but never activates anything.        │
// │    * a compositing manager — grab's overlay requires an owned            │
// │      _NET_WM_CM_S<n> selection, and without it the HUD is a silent no-op. │
// │  Both blocked entire previous branches of this work.                     │
// │                                                                          │
// │  TEARDOWN KILLS RECORDED PIDS ONLY. Never by name, never by pattern: on  │
// │  a shared machine a pattern reaches other people's processes, and since  │
// │  this program's own command line contains the names it would match, its  │
// │  own as well.                                                            │
// └──────────────────────────────────────────────────────────────────────────┘

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <grab/screen.hpp>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ladder::host
{

    constexpr int         display_wait_ms    = 100;
    constexpr int         display_wait_tries = 100;
    constexpr int         service_settle_ms  = 800;
    constexpr int         browser_wait_ms    = 500;
    // How long to give the window manager after an activation request before
    // trusting that input aimed at the browser reaches the browser.
    constexpr int         raise_settle_ms    = 400;
    constexpr int         browser_wait_tries = 60;
    constexpr int         teardown_grace_ms  = 300;
    constexpr const char* default_display    = ":77";
    constexpr const char* default_screen     = "1600x1200";
    // Fits inside a typical laptop panel with room for the operator's own
    // window decorations around the nested display.
    constexpr const char* watch_screen = "1400x900";
    // Every authored page title ends "... — Ground pNN", so this substring
    // identifies OUR browser window and nothing else on the desktop.

    // Every service is started by this process and remembered by pid. Teardown
    // signals those pids and nothing else: no matching by name or command
    // line, which on a shared machine would reach other people's processes —
    // and, since this program's own command line contains the names it would
    // be matching, its own.

    class Child
    {
        public:

            Child()               = default;

            Child( const Child& ) = delete;
            Child&
            operator=( const Child& ) = delete;

            Child( Child&& other ) noexcept :
                pid_( other.pid_ ),
                name_( std::move( other.name_ ) )
            {
                other.pid_ = -1;
            }

            Child&
            operator=( Child&& other ) noexcept
            {
                if( this != &other )
                {
                    stop();
                    pid_       = other.pid_;
                    name_      = std::move( other.name_ );
                    other.pid_ = -1;
                }
                return *this;
            }

            ~Child()
            {
                stop();
            }

            // Spawns argv[0] with the given extra environment. `out_fd`, when
            // not null, receives a read end for the child's stdout.
            [[nodiscard]]
            bool
            start( std::string_view                           name,
                   const std::vector<std::string>&            argv,
                   const std::vector<std::pair<std::string,
                                               std::string>>& env,
                   int*                                       out_fd   = nullptr,
                   const std::string&                         log_path = {} )
            {
                std::array<int, 2> pipe_fds{ -1, -1 };
                if( out_fd != nullptr && ::pipe( pipe_fds.data() ) != 0 )
                {
                    return false;
                }

                const ::pid_t forked = ::fork();
                if( forked < 0 )
                {
                    return false;
                }
                if( forked == 0 )
                {
                    // Child. Only async-signal-safe-ish work here, then exec.
                    // Services are chatty (dbus activation, portal warnings,
                    // Firefox noise) and none of it is this demo's report, so
                    // every child's stderr is discarded. Only the one child we
                    // ask a question of keeps a stdout, and only to answer it.
                    // A named log when the caller wants the child's own words
                    // kept (the browser), /dev/null otherwise.
                    const int null_fd = log_path.empty()
                                          ? ::open( "/dev/null", O_WRONLY )
                                          : ::open( log_path.c_str(),
                                                    O_WRONLY | O_CREAT | O_TRUNC,
                                                    0644 );
                    if( out_fd != nullptr )
                    {
                        ::close( pipe_fds[0] );
                        ( void )::dup2( pipe_fds[1], STDOUT_FILENO );
                        ::close( pipe_fds[1] );
                    }
                    else if( null_fd >= 0 )
                    {
                        ( void )::dup2( null_fd, STDOUT_FILENO );
                    }
                    if( null_fd >= 0 )
                    {
                        ( void )::dup2( null_fd, STDERR_FILENO );
                        ::close( null_fd );
                    }
                    for( const auto& [key, value] : env )
                    {
                        ( void )::setenv( key.c_str(), value.c_str(), 1 );
                    }
                    std::vector<char*> raw;
                    raw.reserve( argv.size() + 1U );
                    for( const auto& piece : argv )
                    {
                        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
                        raw.push_back( const_cast<char*>( piece.c_str() ) );
                    }
                    raw.push_back( nullptr );
                    ( void )::execvp( raw[0], raw.data() );
                    ::_exit( 127 );
                }

                pid_  = forked;
                name_ = name;
                if( out_fd != nullptr )
                {
                    ::close( pipe_fds[1] );
                    *out_fd = pipe_fds[0];
                }
                return true;
            }

            [[nodiscard]]
            bool
            alive() const noexcept
            {
                return pid_ > 0 && ::kill( pid_, 0 ) == 0;
            }

            // SIGTERM, then SIGKILL if it is ignored.
            //
            // A blocking waitpid after SIGTERM alone hangs the whole teardown
            // on any child that declines to exit — Firefox in particular takes
            // its time, and a browser mid-shutdown can outlive the display it
            // was drawing on. Escalating after a grace period means teardown
            // always terminates, which matters because these are the only
            // processes that know they should die.
            void
            stop() noexcept
            {
                if( pid_ <= 0 )
                {
                    return;
                }
                constexpr int grace_tries = 40;
                constexpr int grace_ms    = 50;

                ( void )::kill( pid_, SIGTERM );
                for( int attempt = 0; attempt < grace_tries; ++attempt )
                {
                    int        status = 0;
                    const auto seen   = ::waitpid( pid_, &status, WNOHANG );
                    if( seen == pid_ || seen < 0 )
                    {
                        pid_ = -1;
                        return;
                    }
                    ::usleep( static_cast<unsigned int>( grace_ms ) * 1'000U );
                }
                ( void )::kill( pid_, SIGKILL );
                int status = 0;
                ( void )::waitpid( pid_, &status, 0 );
                pid_ = -1;
            }

            [[nodiscard]]
            ::pid_t
            pid() const noexcept
            {
                return pid_;
            }

        private:

            ::pid_t     pid_{ -1 };
            std::string name_;
    };

    class Host
    {
        public:

            // `marker` is a substring of the title the authored page sets. It
            // is how start() recognises OUR browser window rather than whatever
            // else is mapped — on a bare Xvfb the first window happens to be
            // ours, on a real desktop it is the desktop, and taking the first
            // one is a bug this harness already shipped once.
            Host( std::string           display,
                  std::filesystem::path run_dir,
                  std::string           screen,
                  std::string           host_display,
                  bool                  attach,
                  std::string           marker ) :
                display_( std::move( display ) ),
                run_dir_( std::move( run_dir ) ),
                screen_( std::move( screen ) ),
                host_display_( std::move( host_display ) ),
                marker_( std::move( marker ) ),
                attach_( attach )
            {
            }

            [[nodiscard]]
            bool
            start( const std::string& url );

            [[nodiscard]]
            const std::string&
            bus() const noexcept
            {
                return bus_address_;
            }

            void
            stop()
            {
                // Reverse order: the browser first so it can flush its profile,
                // then the a11y stack, then the display stack underneath it.
                firefox_.stop();
                registryd_.stop();
                atspi_.stop();
                dbus_.stop();
                xcompmgr_.stop();
                openbox_.stop();
                xvfb_.stop();
            }

            // The browser's top-level window, looked up FRESH by the title
            // marker. Fresh because a WindowSummary goes stale the moment the
            // window moves — and on a live desktop (--session) the operator
            // can move it. Everything an example computes from screen
            // geometry — parks, fold checks, wheel positions — must come from
            // here, never from the authored viewport: the authored numbers
            // are page-sized and say nothing about WHERE the window manager
            // put the window.
            [[nodiscard]]
            std::optional<grab::WindowSummary>
            browser_window() const
            {
                auto screen = grab::Screen::open( display_.c_str() );
                if( !screen.has_value() )
                {
                    return std::nullopt;
                }
                auto listed = screen->windows();
                if( !listed.has_value() )
                {
                    return std::nullopt;
                }
                for( const auto& window : *listed )
                {
                    if( window.title.contains( marker_ ) )
                    {
                        return window;
                    }
                }
                return std::nullopt;
            }

        private:

            [[nodiscard]]
            std::filesystem::path
            browser_log_path() const
            {
                return run_dir_ / "firefox.log";
            }

            void
            report_browser_log() const;

            [[nodiscard]]
            bool
            await_display();

            // "1600x1200" -> "1600,1200" for Firefox's --window-size.
            [[nodiscard]]
            std::string
            window_size_arg() const
            {
                std::string arg   = screen_;
                const auto  cross = arg.find( 'x' );
                if( cross != std::string::npos )
                {
                    arg[cross] = ',';
                }
                return arg;
            }

            [[nodiscard]]
            std::vector<std::pair<std::string,
                                  std::string>>
                                  child_env() const;

            std::string           display_;
            std::filesystem::path run_dir_;
            // "WIDTHxHEIGHT" — the nested screen size, shared by the X server
            // and the browser window so the page is never letterboxed.
            std::string           screen_;
            // The operator's display, used for ONE thing: giving Xephyr
            // somewhere to open its window. Empty means headless (Xvfb).
            std::string           host_display_;
            std::string           marker_;
            // Attach to a display that already exists — the operator's own
            // session — instead of creating one. Nothing about their session is
            // started, reconfigured or torn down.
            bool                  attach_{};
            std::string           bus_address_;

            Child                 xvfb_;
            Child                 openbox_;
            Child                 xcompmgr_;
            Child                 dbus_;
            Child                 atspi_;
            Child                 registryd_;
            Child                 firefox_;
    };

    bool
    Host::await_display()
    {
        for( int attempt = 0; attempt < display_wait_tries; ++attempt )
        {
            if( auto screen = grab::Screen::open( display_.c_str() );
                screen.has_value() )
            {
                return true;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds{ display_wait_ms } );
        }
        return false;
    }

    std::vector<std::pair<std::string,
                          std::string>>
    Host::child_env() const
    {
        return {
            {                    "DISPLAY",          display_},
            {   "DBUS_SESSION_BUS_ADDRESS",      bus_address_},
            {        "GNOME_ACCESSIBILITY",               "1"},
            {                "GTK_MODULES", "gail:atk-bridge"},
            {"MOZ_DISABLE_CONTENT_SANDBOX",               "1"},
            {                       "HOME", run_dir_.string()},
        };
    }

    bool
    Host::start( const std::string& url )
    {
        std::error_code failure;
        std::filesystem::create_directories( run_dir_, failure );

        if( attach_ )
        {
            // The operator's session already has an X server, a window manager
            // and a compositing manager. Starting our own would be the thing
            // they asked us not to do.
            if( auto existing = grab::Screen::open( display_.c_str() );
                !existing.has_value() )
            {
                std::cerr << "cannot open " << display_ << " — is that your DISPLAY?\n";
                return false;
            }
            std::cout << "  display    " << display_
                      << " (yours — not created, not reconfigured)\n";
        }
        else
        {
            // Refuse to share a display. Reusing one someone else started would
            // mean tearing down services this process does not own, and would
            // put an unknown set of accessible applications on the a11y bus —
            // the ambiguity that made "the first document on the tree"
            // meaningless.
            if( auto existing = grab::Screen::open( display_.c_str() );
                existing.has_value() )
            {
                std::cerr << "display " << display_
                          << " is already in use; pick a free one with --display"
                             ", or --session to drive the one you are using\n";
                return false;
            }
        }

        // ── Xvfb ────────────────────────────────────────────────────────────
        // -extension GLX: the NVIDIA EGL vendor library segfaults Xvfb during
        // GLX init on some hosts, and nothing here needs GL.
        if( !attach_ )
        {
            // WATCH MODE: Xephyr is a nested X server that opens as an ordinary
            // WINDOW on the operator's desktop while being a genuinely separate
            // display. Everything the crawler does — cursor motion, clicks,
            // overlays, capture — happens on that nested display, so it is visible
            // without ever being synthesized onto the operator's own session.
            //
            // This is the single point where the demo touches the host display,
            // and only to ask it to map a window. It never reads its pixels,
            // never moves its pointer, and never changes its focus.
            const bool               watching = !host_display_.empty();
            const char*              server   = watching ? "Xephyr" : "Xvfb";

            std::vector<std::string> server_argv{ server, display_ };
            if( watching )
            {
                server_argv.insert( server_argv.end(),
                                    { "-screen",
                                      screen_,
                                      "-title",
                                      "grab stage (nested display " + display_ + ")",
                                      "-resizeable",
                                      "-ac" } );
            }
            else
            {
                // -extension GLX: the NVIDIA EGL vendor library segfaults Xvfb
                // during GLX init on some hosts, and nothing here needs GL.
                server_argv.insert(
                    server_argv.end(),
                    { "-screen", "0", screen_ + "x24", "-ac", "-extension", "GLX" }
                );
            }
            server_argv.insert( server_argv.end(),
                                { "+extension",
                                  "COMPOSITE",
                                  "+extension",
                                  "RANDR",
                                  "+extension",
                                  "XTEST" } );

            std::vector<std::pair<std::string, std::string>> server_env;
            if( watching )
            {
                server_env.emplace_back( "DISPLAY", host_display_ );
            }

            if( !xvfb_.start( server, server_argv, server_env ) || !await_display() )
            {
                std::cerr << "cannot start " << server << " on " << display_;
                if( watching )
                {
                    std::cerr << " (host display " << host_display_ << ")";
                }
                std::cerr << '\n';
                return false;
            }
            std::cout << "  " << server << ( watching ? "     " : "       " ) << display_
                      << " " << screen_ << " (pid " << xvfb_.pid() << ")";
            if( watching )
            {
                std::cout << "  <- watch this window";
            }
            std::cout << '\n';

            // ── window manager ──────────────────────────────────────────────────
            if( !openbox_.start( "openbox",
                                 {
                                     "openbox"
            },
                                 { { "DISPLAY", display_ } } ) )
            {
                std::cerr << "cannot start openbox (apt install openbox)\n";
                return false;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds{ service_settle_ms }
            );
            if( !openbox_.alive() )
            {
                std::cerr << "openbox exited immediately; is it installed?\n";
                return false;
            }
            std::cout << "  openbox    focus manager (pid " << openbox_.pid() << ")\n";

            // ── compositing manager ─────────────────────────────────────────────
            if( !xcompmgr_.start( "xcompmgr",
                                  {
                                      "xcompmgr",
                                      "-c"
            },
                                  { { "DISPLAY", display_ } } ) )
            {
                std::cerr << "cannot start xcompmgr (apt install xcompmgr)\n";
                return false;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds{ service_settle_ms }
            );
            if( !xcompmgr_.alive() )
            {
                std::cerr << "xcompmgr exited immediately; is it installed?\n";
                return false;
            }
            std::cout << "  xcompmgr   owns _NET_WM_CM_S0 (pid " << xcompmgr_.pid()
                      << ")\n";
        }

        // Everything below runs in BOTH modes. Even when attached to the
        // operator's session the demo brings up its own session bus, its own
        // a11y bus and its own browser: that is what keeps the accessibility
        // tree it reads down to the one browser it launched, instead of every
        // accessible application on a busy desktop.

        // ── session bus ─────────────────────────────────────────────────────
        // --nofork so the daemon IS our direct child and its pid is known;
        // the address comes back over a pipe rather than a temp file.
        int bus_fd = -1;
        if( !dbus_.start(
                "dbus-daemon",
                { "dbus-daemon", "--session", "--nofork", "--print-address=1" },
                {},
                &bus_fd
            ) )
        {
            std::cerr << "cannot start dbus-daemon\n";
            return false;
        }
        {
            std::string           line;
            std::array<char, 512> buffer{};
            const auto read_bytes = ::read( bus_fd, buffer.data(), buffer.size() );
            ::close( bus_fd );
            if( read_bytes > 0 )
            {
                line.assign( buffer.data(), static_cast<std::size_t>( read_bytes ) );
            }
            const auto newline = line.find( '\n' );
            bus_address_ =
                newline == std::string::npos ? line : line.substr( 0U, newline );
        }
        if( bus_address_.empty() )
        {
            std::cerr << "dbus-daemon did not report an address\n";
            return false;
        }
        std::cout << "  dbus       " << bus_address_ << '\n';

        // ── accessibility bus ───────────────────────────────────────────────
        const std::vector<std::pair<std::string, std::string>> a11y_env{
            {                 "DISPLAY",     display_},
            {"DBUS_SESSION_BUS_ADDRESS", bus_address_},
        };
        if( !atspi_.start( "at-spi-bus-launcher",
                           { "/usr/libexec/at-spi-bus-launcher",
                             "--launch-immediately" },
                           a11y_env ) )
        {
            std::cerr << "cannot start at-spi-bus-launcher\n";
            return false;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds{ service_settle_ms } );
        if( !registryd_.start( "at-spi2-registryd",
                               { "/usr/libexec/at-spi2-registryd" },
                               a11y_env ) )
        {
            std::cerr << "cannot start at-spi2-registryd\n";
            return false;
        }
        std::cout << "  at-spi     accessibility bus up\n";

        // ── browser profile ─────────────────────────────────────────────────
        const auto profile = run_dir_ / "ffprofile";
        std::filesystem::create_directories( profile, failure );
        {
            std::ofstream prefs{ profile / "user.js", std::ios::binary };
            prefs << R"(user_pref("accessibility.force_disabled", 0);
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.startup.homepage_override.mstone", "ignore");
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("datareporting.healthreport.uploadEnabled", false);
user_pref("toolkit.telemetry.enabled", false);
user_pref("browser.aboutwelcome.enabled", false);
user_pref("browser.startup.firstrunSkipsHomepage", true);
user_pref("trailhead.firstrun.didSeeAboutWelcome", true);
user_pref("browser.sessionstore.resume_from_crash", false);
user_pref("browser.tabs.warnOnClose", false);
user_pref("app.update.auto", false);
user_pref("extensions.autoDisableScopes", 0);
user_pref("browser.newtabpage.enabled", false);
user_pref("browser.uitour.enabled", false);
user_pref("layout.css.devPixelsPerPx", "1.0");
user_pref("browser.link.open_newwindow", 1);
user_pref("general.smoothScroll", false);
)";
        }

        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        const char* const firefox_override = std::getenv( "SPIDER_FIREFOX" );
        const std::string firefox_binary =
            firefox_override != nullptr
                ? std::string{ firefox_override }
                : std::string{ "/snap/firefox/current/usr/lib/firefox/firefox" };

        // --new-instance is what stops an already-running Firefox from
        // swallowing this launch: without it the URL is handed to the
        // operator's existing browser over remoting, our child exits at once,
        // and nothing we can drive ever appears.
        if( !firefox_.start( "firefox",
                             { firefox_binary,
                               "--new-instance",
                               "--profile",
                               profile.string(),
                               "--window-size",
                               window_size_arg(),
                               "--new-window",
                               url },
                             child_env(),
                             nullptr,
                             browser_log_path().string() ) )
        {
            std::cerr << "cannot start " << firefox_binary
                      << " (override with SPIDER_FIREFOX)\n";
            return false;
        }

        // Wait for OUR browser, identified by the title the generator authored.
        //
        // Taking windows().front() was wrong in a way that only shows on a real
        // desktop: on an empty Xvfb the first window IS our browser, so the
        // check passed for the wrong reason. On the operator's session the
        // first window is the desktop ("Desktop Icons"), so the host reported
        // "firefox    Desktop Icons" and returned success before the browser
        // had started — or, as it turned out, whether it started at all. Every
        // later failure then pointed at the a11y stack instead of here.
        //
        // A readiness check that can succeed without the thing being ready is
        // worse than no check.
        for( int attempt = 0; attempt < browser_wait_tries; ++attempt )
        {
            if( !firefox_.alive() )
            {
                std::cerr << "firefox exited immediately.\n";
                report_browser_log();
                return false;
            }
            if( auto screen = grab::Screen::open( display_.c_str() );
                screen.has_value() )
            {
                if( auto listed = screen->windows(); listed.has_value() )
                {
                    for( const auto& window : *listed )
                    {
                        if( window.title.contains( marker_ ) )
                        {
                            std::cout << "  firefox    " << window.title << " (pid "
                                      << firefox_.pid() << ")\n";
                            // Raise it THE MOMENT it maps. On an owned display
                            // this is a formality; on a live desktop
                            // (--session) a new window can open behind, and
                            // then every wheel notch and every click lands on
                            // whatever is in front of it. Activation is a
                            // REQUEST to the window manager — asynchronous and
                            // legitimately refusable — so this asks, waits a
                            // beat, and reports, rather than assuming.
                            if( auto raised = screen->activate_window( window.id );
                                raised.has_value() )
                            {
                                std::this_thread::sleep_for(
                                    std::chrono::milliseconds{ raise_settle_ms }
                                );
                                std::cout << "  raise      browser raised\n";
                            }
                            else
                            {
                                std::cout << "  raise      REFUSED — input may "
                                             "land on whatever is in front\n";
                            }
                            return true;
                        }
                    }
                }
            }
            std::this_thread::sleep_for( std::chrono::milliseconds{ browser_wait_ms } );
        }

        std::cerr << "firefox never mapped a window titled \"" << marker_ << "\" on "
                  << display_ << ".\n";
        if( auto screen = grab::Screen::open( display_.c_str() ); screen.has_value() )
        {
            if( auto listed = screen->windows(); listed.has_value() )
            {
                std::cerr << "windows actually present:\n";
                for( const auto& window : *listed )
                {
                    std::cerr << "  [" << window.wm_class << "] " << window.title
                              << '\n';
                }
            }
        }
        report_browser_log();
        return false;
    }

    // Prints the tail of the browser's own output. Every other service is
    // silenced because its chatter is noise, but when the browser is the thing
    // that failed, its log is the only evidence there is.
    void
    Host::report_browser_log() const
    {
        constexpr std::size_t tail_lines = 15U;

        std::ifstream         log{ browser_log_path() };
        if( !log )
        {
            std::cerr << "(no browser log at " << browser_log_path() << ")\n";
            return;
        }
        std::vector<std::string> lines;
        std::string              line;
        while( std::getline( log, line ) )
        {
            lines.push_back( line );
        }
        std::cerr << "last of " << browser_log_path() << ":\n";
        const std::size_t first =
            lines.size() > tail_lines ? lines.size() - tail_lines : 0U;
        for( std::size_t index = first; index < lines.size(); ++index )
        {
            std::cerr << "  " << lines[index] << '\n';
        }
    }

}    // namespace ladder::host
