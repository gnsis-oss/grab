// ┌──────────────────────────────────────────────────────────────────────────┐
// │  stage_button — rung 4 of the capability ladder.                         │
// │                                                                          │
// │  ONE page. ONE large button at an authored rectangle. The cursor is       │
// │  shown where it is going, travels there on a human trajectory, presses,   │
// │  and the result is checked THREE ways that cannot cover for each other:   │
// │                                                                          │
// │    a11y    the button's accessible name flips IDLE -> CLICKED             │
// │    pixel   the button's fill flips from one authored colour to another    │
// │    device  the press happened INSIDE the button, read back from the X     │
// │            server rather than from what this program believes             │
// │                                                                          │
// │  Any one of those alone can be satisfied by an accident. The a11y name    │
// │  can change because the page reloaded; the pixels can change because      │
// │  something repainted; the cursor can be inside the rect having clicked    │
// │  nothing. Together they are a click.                                      │
// │                                                                          │
// │  --trail parks the cursor bottom-left and draws its path on the overlay   │
// │  as it sweeps up to the button, so the stroke lies directly OVER the      │
// │  button when the press lands. That is the click-through claim: overlay    │
// │  pixels above the page, and the press below still reaching it. Nothing    │
// │  new is asserted for it — if the trail swallowed the press, all three     │
// │  checks above would fail at once, which is a stronger statement than a    │
// │  fourth check that only watches itself. The strokes are retired right     │
// │  after the release, so the observers read the page and not the overlay.   │
// │                                                                          │
// │    stage_button                 headless, on a display it creates          │
// │    stage_button --session       the display you are already on            │
// │    stage_button --trail         with the cursor trail drawn over the page │
// │    stage_button --trail --watch same, in a Xephyr WINDOW you can watch    │
// │    stage_button --keep          leave the session up afterwards           │
// │                                                                          │
// │  --watch is how to see this on a real desktop. The nested display is a    │
// │  genuinely separate X server that merely happens to be drawn in a window: │
// │  the cursor, the clicks and the overlay all stay inside it, so nothing is │
// │  ever synthesized onto the session you are sitting in front of.           │
// └──────────────────────────────────────────────────────────────────────────┘

#include "support/host.hpp"
#include "support/motion/noise.hpp"
#include "support/overlay_align.hpp"
#include "support/pixel.hpp"
#include "support/motion/trajectory.hpp"
#include "support/stage/assert.hpp"
#include "support/stage/probe.hpp"
#include "support/stage/scene.hpp"
#include "support/surface.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <grab/event.hpp>
#include <grab/input.hpp>
#include <grab/locator.hpp>
#include <grab/overlay.hpp>
#include <grab/role.hpp>
#include <grab/screen.hpp>
#include <grab/session.hpp>
#include <grab/watch.hpp>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace
{

    namespace stage  = ladder::view::stage;
    namespace motion = ladder::view::motion;
    namespace view   = ladder::view;
    namespace pixel  = ladder::view::pixel;

    // ── The authored page ───────────────────────────────────────────────────
    //
    // Absolutely positioned, explicit width and height, box-sizing:border-box.
    // The border box is then exactly what is written here regardless of which
    // fonts are installed — an inline element sized by its own glyphs would
    // make every number below a guess about font metrics.

    constexpr int           button_x      = 240;
    constexpr int           button_y      = 220;
    constexpr int           button_w      = 420;
    constexpr int           button_h      = 160;
    constexpr const char*   idle_label    = "IDLE";
    constexpr const char*   clicked_label = "CLICKED";
    // Chosen far apart in RGB and clear of the overlay palette, so "the button
    // changed" is not a judgement call about a shade.
    constexpr const char*   idle_fill      = "#1d4e89";
    constexpr const char*   clicked_fill   = "#c1121f";
    constexpr const char*   title_marker   = "Stage Button";

    constexpr std::uint32_t primary_button = 1U;    // X11 primary pointer button code

    constexpr int           viewport_w     = 1'000;
    constexpr int           viewport_h     = 700;

    constexpr const char*   subject        = "btn_main";

    constexpr grab::overlay::Color cyan{ .r = 0U, .g = 217U, .b = 255U, .a = 242U };
    constexpr grab::overlay::Color green{ .r = 51U, .g = 255U, .b = 89U, .a = 242U };
    constexpr grab::overlay::Color amber{ .r = 255U, .g = 184U, .b = 26U, .a = 242U };

    constexpr double               stroke_px       = 3.0;
    constexpr double               trail_stroke_px = 2.0;
    // Where --trail parks the cursor before the approach, as FRACTIONS of the
    // browser window's live frame. The pointer's resting place on a fresh
    // display is about 100 px from the button, and a trail over 100 px of
    // travel is a squiggle that shows nothing -- measured, on the first run
    // of this code. Bottom-left of the WINDOW, clear of the button rect, so
    // the sweep crosses the page diagonally. Fractions rather than absolute
    // screen coordinates, because on a real desktop the window manager
    // decides where the window is — an absolute park can sit over another
    // application entirely.
    constexpr double               trail_origin_fx = 0.08;
    constexpr double               trail_origin_fy = 0.88;
    // Do not start an overlay round trip when the next cursor sample is nearly
    // due. Overlay::add measures ~645 us mean but ~5.1 ms worst on this
    // machine; the trajectory is the thing under test and the trail is
    // decoration drawn on top of it, so the trail yields, never the motion.
    constexpr auto                 trail_slack = std::chrono::milliseconds{ 8 };

    constexpr std::uint64_t        seed        = 0X5'D1'DE'00'00ULL;
    constexpr int                  settle_ms   = 700;
    constexpr int                  announce_ms = 900;
    constexpr int                  react_ms    = 900;
    constexpr int                  poll_ms     = 200;
    constexpr int    poll_tries   = 150;    // 30 s: Firefox builds its a11y tree lazily
    constexpr double colour_match = 40.0;

    [[nodiscard]]
    std::string
    page_html()
    {
        // The button reports its own state in BOTH channels at once: the
        // accessible name and the fill colour change together, so the two
        // observers below are reading one event through two instruments.
        return std::string{ "<!doctype html>\n"
                            "<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
                            "<title>Stage Button — grab</title>\n"
                            "<style>\n"
                            "  html,body{margin:0;padding:0;background:#f4f4f4;}\n"
                            "  #btn_main{position:absolute;box-sizing:border-box;" } +
               "left:" +
               std::to_string( button_x ) +
               "px;top:" +
               std::to_string( button_y ) +
               "px;width:" +
               std::to_string( button_w ) +
               "px;height:" +
               std::to_string( button_h ) +
               "px;"
               "background:" +
               idle_fill +
               ";color:#fff;border:0;font:600 40px sans-serif;}\n"
               "</style></head><body>\n"
               "<button id=\"btn_main\" aria-label=\"" +
               idle_label +
               "\">" +
               idle_label +
               "</button>\n"
               "<script>\n"
               "  var b=document.getElementById('btn_main');\n"
               "  b.addEventListener('click',function(){\n"
               "    b.style.background='" +
               clicked_fill +
               "';\n"
               "    b.setAttribute('aria-label','" +
               clicked_label +
               "');\n"
               "    b.textContent='" +
               clicked_label +
               "';\n"
               "  });\n"
               "</script></body></html>\n";
    }

    [[nodiscard]]
    stage::Scene
    build_scene()
    {
        stage::Scene scene;
        scene.id_ = "button";
        scene.pages_.push_back( stage::ScenePage{
            .name_   = "button",
            .html_   = page_html(),
            .marker_ = title_marker
        } );
        scene.viewport_ = stage::ViewportSpec{
            .viewport_w_ = viewport_w,
            .viewport_h_ = viewport_h,
            .document_h_ = viewport_h
        };
        scene.frames_ = { "00-authored", "01-overlay", "02-approach", "03-after" };

        // DECLARED BEFORE THE ACT. Three observers, three independent ways for
        // this to fail.
        scene.expect_ = {
            stage::Expectation{
                               .name_      = "overlay_is_live",
                               .observe_   = stage::Observe::Capability,
                               .subject_   = "overlay",
                               .value_     = "live",
                               .tolerance_ = 0.0,
                               .low_       = 0.0,
                               .high_      = 0.0,
                               .ranged_    = false},
            stage::Expectation{
                               .name_      = "a11y_reports_clicked",
                               .observe_   = stage::Observe::A11yName,
                               .subject_   = subject,
                               .value_     = clicked_label,
                               .tolerance_ = 0.0,
                               .low_       = 0.0,
                               .high_      = 0.0,
                               .ranged_    = false},
            stage::Expectation{
                               .name_      = "pixels_flipped",
                               .observe_   = stage::Observe::PixelColour,
                               .subject_   = subject,
                               .value_     = "changed",
                               .tolerance_ = 0.0,
                               .low_       = 0.0,
                               .high_      = 0.0,
                               .ranged_    = false},
            stage::Expectation{
                               .name_      = "press_inside_button",
                               .observe_   = stage::Observe::CursorPosition,
                               .subject_   = subject,
                               .value_     = "inside",
                               .tolerance_ = 0.0,
                               .low_       = 0.0,
                               .high_      = 0.0,
                               .ranged_    = false},
            // The browser lays the button out where the page said to. Checked
            // as SIZE, which is invariant under the chrome offset: the a11y
            // rect is in SCREEN space and the authored rect in PAGE space, and
            // comparing those directly is how the first run of this rung
            // "failed" a click that had in fact landed dead centre.
            stage::Expectation{
                               .name_    = "button_size_as_authored",
                               .observe_ = stage::Observe::A11yBounds,
                               .subject_ = subject,
                               .value_ = std::to_string( button_w ) + "x" + std::to_string( button_h ),
                               .tolerance_ = 0.0,
                               .low_       = 0.0,
                               .high_      = 0.0,
                               .ranged_    = false},
            stage::Expectation{
                               .name_      = "button_click_observed",
                               .observe_   = stage::Observe::ButtonClick,
                               .subject_   = subject,
                               .value_     = "clicked",
                               .tolerance_ = 0.0,
                               .low_       = 0.0,
                               .high_      = 0.0,
                               .ranged_    = false},
        };
        return scene;
    }

    [[nodiscard]]
    std::string
    why( const grab::Error& error )
    {
        std::string text = error.message;
        if( !error.capability.empty() )
        {
            text += " [capability " + error.capability + "]";
        }
        return text;
    }


    // The button's accessible name, and its screen rect, read live.
    struct Live
    {
            std::string             name_;
            view::ViewRect          rect_{};
            // The space the a11y bounds were reported IN. Kept because it is
            // not necessarily the space the overlay draws in or the space
            // Input drives in, and on a 1x display that difference is
            // invisible -- the numbers coincide.
            grab::CoordinateSpaceId space_{};
    };

    [[nodiscard]]
    std::optional<Live>
    resolve_button( grab::Session& session )
    {
        if( auto synced = session.resync(); !synced.has_value() )
        {
            return std::nullopt;
        }
        // Scoped to the DOCUMENT. A bare role(button) also matches the window
        // manager's own titlebar decorations — the first run of this rung
        // resolved openbox's "Minimize" at (886,-12) and drove the cursor
        // there, which is a click on a real button and entirely the wrong one.
        auto matches =
            session.resolve_all( grab::sel::role( grab::role::button )
                                     .and_( grab::sel::descendant_of(
                                         grab::sel::role( grab::role::document )
                                     ) ) );
        if( !matches.has_value() )
        {
            return std::nullopt;
        }
        for( const grab::Match& match : *matches )
        {
            auto described = session.describe( match );
            if( !described.has_value() )
            {
                continue;
            }
            const auto& info = *described;
            if( info.bounds.w <= 0.0 || info.bounds.h <= 0.0 )
            {
                continue;
            }
            return Live{
                .name_ = info.name,
                .rect_ =
                    view::ViewRect{
                                   .x_ = info.bounds.x,
                                   .y_ = info.bounds.y,
                                   .w_ = info.bounds.w,
                                   .h_ = info.bounds.h
                    },
                .space_ = info.bounds.space
            };
        }
        return std::nullopt;
    }

    struct Options
    {
            std::string           display = ":72";
            std::filesystem::path out     = "stage-button";
            // Non-empty runs the nested display in a Xephyr WINDOW on this
            // display instead of headless. The demo still drives only its own
            // nested display; the host is asked to map a window and nothing
            // else -- no pointer, no focus, no pixels read.
            std::string           host_display;
            bool                  attach = false;
            bool                  keep   = false;
            bool                  trail  = false;
    };

    void
    usage()
    {
        std::cout << "stage_button — rung 4: overlay, approach, click, verify\n\n"
                     "  --display :N   display to create (ignored with --session)\n"
                     "  --out DIR      where the page, PNGs and scorecard land\n"
                     "  --session      drive the display you are already on\n"
                     "  --watch        show the nested display in a window here\n"
                     "  --trail        draw the cursor's path over the page\n"
                     "  --keep         leave the session up afterwards\n"
                     "  --help         this text\n";
    }

    [[nodiscard]]
    std::optional<Options>
    parse( int    argc,
           char** argv,
           int&   code )
    {
        code = 0;
        Options options;
        for( int index = 1; index < argc; ++index )
        {
            const std::string_view flag{ argv[index] };
            bool                   missing = false;
            const auto             value   = [&]() -> std::string
            {
                if( index + 1 >= argc )
                {
                    missing = true;
                    return {};
                }
                return std::string{ argv[++index] };
            };
            const auto bad = [&]( std::string_view reason ) -> std::optional<Options>
            {
                std::cerr << "stage_button: " << flag << ' ' << reason << "\n\n";
                usage();
                code = 2;
                return std::nullopt;
            };

            if( flag == "--help" || flag == "-h" )
            {
                usage();
                return std::nullopt;
            }
            if( flag == "--display" )
            {
                options.display = value();
                if( missing || options.display.empty() )
                {
                    return bad( "requires a display, e.g. :72" );
                }
            }
            else if( flag == "--out" )
            {
                const std::string raw = value();
                if( missing || raw.empty() )
                {
                    return bad( "requires a directory" );
                }
                // Absolute from the start: the host points Firefox's HOME at
                // this directory, and Firefox rejects a relative HOME with
                // "Profile Missing" — a failure that names everything except
                // the actual cause.
                options.out = std::filesystem::absolute( raw );
            }
            else if( flag == "--session" )
            {
                options.attach = true;
                // NOLINTNEXTLINE(concurrency-mt-unsafe)
                const char* const ambient = std::getenv( "DISPLAY" );
                if( ambient == nullptr || ambient[0] == '\0' )
                {
                    std::cerr << "stage_button: --session needs DISPLAY set\n";
                    code = 2;
                    return std::nullopt;
                }
                options.display = ambient;
            }
            else if( flag == "--trail" )
            {
                options.trail = true;
            }
            else if( flag == "--watch" )
            {
                // Default to the display we were launched from.
                // NOLINTNEXTLINE(concurrency-mt-unsafe)
                const char* const ambient = std::getenv( "DISPLAY" );
                options.host_display      = ambient != nullptr && ambient[0] != '\0'
                                              ? std::string{ ambient }
                                              : std::string{ ":0" };
            }
            else if( flag == "--keep" )
            {
                options.keep = true;
            }
            else
            {
                return bad( "is not a recognised flag" );
            }
        }
        // --session drives a display that already exists, so there is no nested
        // display for Xephyr to host. Ignoring one of the two would leave the
        // operator watching a window that is not where the work is happening.
        if( options.attach && !options.host_display.empty() )
        {
            std::cerr
                << "stage_button: --watch and --session are mutually exclusive\n\n";
            usage();
            code = 2;
            return std::nullopt;
        }
        return options;
    }

}    // namespace

int
main( int    argc,
      char** argv )
{
    // grab's own diagnostics are environment-driven: GRAB_LOG=debug (plus
    // GRAB_LOG_TAGS / GRAB_LOG_FILE) turns the library's logging on without
    // this example carrying a sink of its own.
    std::cout << std::unitbuf;

    int        code   = 0;
    const auto parsed = parse( argc, argv, code );
    if( !parsed.has_value() )
    {
        return code;
    }
    const Options       options = *parsed;

    const stage::Scene  scene   = build_scene();
    stage::Observations seen;

    // ── 1. AUTHOR ───────────────────────────────────────────────────────────
    std::error_code     ignored;
    std::filesystem::create_directories( options.out, ignored );
    const std::filesystem::path page = options.out / "button.html";
    {
        std::ofstream out( page );
        out << scene.pages_.front().html_;
    }
    std::cout << "AUTHOR\n  button    (" << button_x << "," << button_y << " "
              << button_w << "x" << button_h << ") -> " << page << '\n';

    // ── 2. HOST ─────────────────────────────────────────────────────────────
    std::cout << "\nHOST\n";
    ladder::host::Host host{
        options.display,
        options.out,
        std::to_string( viewport_w ) + "x" + std::to_string( viewport_h ),
        options.host_display,
        options.attach,
        title_marker
    };
    if( !host.start( "file://" + std::filesystem::absolute( page ).string() ) )
    {
        std::cerr << "host did not come up\n";
        host.stop();
        return 1;
    }

    // The host started its OWN session bus, and the accessibility bus hangs off
    // it. Without pointing this process at that bus, AT-SPI resolves against
    // whatever bus the shell happened to have: the browser window is visible
    // through X, so the tree looks alive, but it contains NO document — which
    // reads exactly like "the page has no accessibility tree" and sends the
    // investigation to Firefox instead of to one missing environment variable.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    ( void )::setenv( "DBUS_SESSION_BUS_ADDRESS", host.bus().c_str(), 1 );

    int exit_code = 1;
    {
        grab::SessionOptions session_options;
        session_options.display = options.display;
        auto session            = grab::Session::open( session_options );
        if( !session.has_value() )
        {
            std::cerr << "session: " << why( session.error() ) << '\n';
            host.stop();
            return 1;
        }
        auto screen = grab::Screen::open( options.display.c_str() );
        auto input  = grab::Input::open( options.display.c_str() );
        if( !screen.has_value() || !input.has_value() )
        {
            std::cerr << "screen/input unavailable\n";
            host.stop();
            return 1;
        }

        // The reactor must run before the overlay can map or animate. Without
        // it the overlay reports live, every draw returns success, and nothing
        // is ever painted.
        if( auto observing = ( *session )->start_observation(); !observing.has_value() )
        {
            std::cerr << "reactor: " << why( observing.error() ) << '\n';
        }

        // Subscribe to mouse button events (DOWN and UP pairs)
        std::optional<grab::Subscription> button_subscription;
        {
            grab::SubscriptionScope button_scope;
            button_scope.kinds = {
                grab::EventKind::MouseButtonDown,
                grab::EventKind::MouseButtonUp
            };
            auto btn_sub = ( *session )->watch( button_scope );
            if( btn_sub.has_value() )
            {
                button_subscription = std::move( *btn_sub );
            }
            else
            {
                seen.push_back( stage::Observation{
                    .observe_ = stage::Observe::Capability,
                    .subject_ = "button_events",
                    .value_   = why( btn_sub.error() )
                } );
            }
        }

        grab::Overlay*          overlay = nullptr;
        grab::CoordinateSpaceId space{};
        std::string             overlay_state = "unavailable";
        if( auto handle = ( *session )->overlay(); handle.has_value() )
        {
            overlay = *handle;
            if( auto resolved = overlay->space(); resolved.has_value() )
            {
                space         = *resolved;
                overlay_state = "live";
            }
            else
            {
                std::cerr << "overlay: " << why( resolved.error() ) << '\n';
                overlay = nullptr;
            }
        }
        else
        {
            std::cerr << "overlay: " << why( handle.error() ) << '\n';
        }
        seen.push_back( stage::Observation{
            .observe_ = stage::Observe::Capability,
            .subject_ = "overlay",
            .value_   = overlay_state
        } );
        std::cout << "  overlay   " << overlay_state << '\n';

        // ── 3. RESOLVE ──────────────────────────────────────────────────────
        std::optional<Live> live;
        for( int attempt = 0; attempt < poll_tries && !live.has_value(); ++attempt )
        {
            live = resolve_button( **session );
            if( !live.has_value() )
            {
                std::this_thread::sleep_for( std::chrono::milliseconds{ poll_ms } );
            }
        }
        if( !live.has_value() )
        {
            // Say WHAT IS THERE, not just what is missing. "The button never
            // appeared" is the same unhelpful shape as "frontier empty": it
            // names the outcome and hides whether the tree is empty, whether
            // the document loaded, or whether the button is simply a different
            // role than expected.
            std::cerr << "the button never appeared in the accessibility tree\n";
            ( void )( *session )->resync();
            const std::array<std::pair<grab::RoleId, const char*>, 6U> probes{
                { { grab::role::document, "document" },
                 { grab::role::button, "button" },
                 { grab::role::window, "window" },
                 { grab::role::text, "text" },
                 { grab::role::link, "link" },
                 { grab::role::panel, "panel" } }
            };
            for( const auto& [role_id, label] : probes )
            {
                auto found = ( *session )->resolve_all( grab::sel::role( role_id ) );
                std::cerr << "  " << label << ": "
                          << ( found.has_value()
                                   ? std::to_string( found->size() ) + " node(s)"
                                   : "resolve failed — " + why( found.error() ) )
                          << '\n';
            }
            host.stop();
            return 1;
        }
        std::cout << "  button    a11y (" << static_cast<int>( live->rect_.x_ ) << ","
                  << static_cast<int>( live->rect_.y_ ) << " "
                  << static_cast<int>( live->rect_.w_ ) << "x"
                  << static_cast<int>( live->rect_.h_ ) << ") name=\"" << live->name_
                  << "\"\n";
        // The two spaces, side by side. If they differ, every number the a11y
        // tree reports is in different units from the ones Input and Overlay
        // use, and the demo is aiming at a rectangle that is not where it
        // thinks. On a 1x display they coincide and this line reads as noise;
        // on a scaled desktop it is the whole diagnosis.
        std::cout << "  spaces    a11y=" << live->space_.value
                  << " overlay=" << space.value
                  << ( live->space_.value == space.value ? " (same)" : " DIFFERENT" )
                  << '\n';

        // ── 4. DRAW ─────────────────────────────────────────────────────────
        // Where do overlay shapes ACTUALLY land? Measured, at the button.
        const auto omap = ladder::view::align::measure(
            overlay, *screen, space, live->rect_.x_, live->rect_.y_ );
        const auto goal = [&]( const grab::overlay::Color& colour )
        {
            if( overlay == nullptr )
            {
                return;
            }
            const view::ViewRect box_rect = omap.rect( live->rect_ );
            grab::overlay::Shape box{
                .geometry =
                    grab::overlay::Rect{
                                        .bounds =
                            grab::SpaceRect{
                                .x     = box_rect.x_,
                                .y     = box_rect.y_,
                                .w     = box_rect.w_,
                                .h     = box_rect.h_,
                                .space = space
                            }
                    },
                .stroke =
                    grab::overlay::StrokeStyle{ .color = colour, .width_px = stroke_px },
                .fill     = std::nullopt,
                .lifetime = grab::overlay::Persistent{},
                .band     = grab::overlay::Band::Annotation,
                .z        = 0,
            };
            ( void )overlay->add( std::move( box ) );
            ( void )overlay->flush();
        };
        goal( cyan );
        std::this_thread::sleep_for( std::chrono::milliseconds{ announce_ms } );

        auto        before = ( *screen ).display();

        // ── 5. APPROACH AND PRESS ───────────────────────────────────────────
        motion::Rng rng{ seed };
        // Scoped to --trail on purpose. Parking the cursor changes the
        // trajectory, and the plain run's numbers should stay comparable with
        // every earlier one rather than quietly moving under a new flag.
        if( options.trail )
        {
            // Park relative to the window's live frame, wherever the window
            // manager put it. Falls back to the button's own rect if the
            // window list momentarily fails — a short trail beats no park.
            auto trail_park_x = static_cast<std::int16_t>( live->rect_.x_ );
            auto trail_park_y = static_cast<std::int16_t>( live->rect_.y_ );
            if( const auto summary = host.browser_window(); summary.has_value() )
            {
                trail_park_x = static_cast<std::int16_t>(
                    static_cast<double>( summary->bounds.x ) +
                    ( static_cast<double>( summary->bounds.width ) * trail_origin_fx )
                );
                trail_park_y = static_cast<std::int16_t>(
                    static_cast<double>( summary->bounds.y ) +
                    ( static_cast<double>( summary->bounds.height ) * trail_origin_fy )
                );
            }
            ( void )( *input ).move( trail_park_x, trail_park_y );
            std::this_thread::sleep_for( std::chrono::milliseconds{ settle_ms } );
        }
        const auto   start = ( *input ).position();
        motion::Vec2 cursor{
            start.has_value() ? static_cast<double>( start->x ) : 0.0,
            start.has_value() ? static_cast<double>( start->y ) : 0.0
        };
        const motion::Rect
                   aim{ live->rect_.x_, live->rect_.y_, live->rect_.w_, live->rect_.h_ };
        const auto movement =
            motion::plan_move( rng, cursor, aim, motion::MotionConfig{} );

        const double lead =
            movement.samples_.empty() ? 0.0 : movement.samples_.front().t_s_;
        const auto begin       = std::chrono::steady_clock::now();
        const auto deadline_of = [&]( const motion::Sample& at )
        {
            return begin +
                   std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                       std::chrono::duration<double>( at.t_s_ - lead )
                   );
        };

        // Points travelled but not yet drawn. The last drawn point stays at the
        // front of the next batch so consecutive strokes join instead of
        // showing a gap wherever a batch happened to end.
        std::vector<motion::Vec2>           pending;
        // Persistent, not Fade, and every id kept.
        //
        // A Fade sized to keep the whole sweep on screen would still be there
        // when the pixel observer reads the button, and a Fade short enough to
        // be gone by then outlives only the final deceleration -- measured, at
        // 400 ms, as a single dot. Retiring the strokes explicitly after the
        // press gets both: the entire path visible while the click lands, and
        // a clean button underneath when the colour is read.
        std::vector<grab::overlay::ShapeId> trail_ids;
        const auto                          stroke = [&]()
        {
            if( overlay == nullptr || pending.size() < 2U )
            {
                return;
            }
            std::vector<grab::overlay::PathCommand> commands;
            commands.reserve( pending.size() );
            commands.emplace_back( grab::overlay::MoveTo{
                .point = grab::SpacePoint{
                                          .x     = omap.x( pending.front().x_ ),
                                          .y     = omap.y( pending.front().y_ ),
                                          .space = space
                }
            } );
            for( std::size_t step = 1U; step < pending.size(); ++step )
            {
                commands.emplace_back( grab::overlay::LineTo{
                    .point = grab::SpacePoint{
                                              .x     = omap.x( pending[step].x_ ),
                                              .y     = omap.y( pending[step].y_ ),
                                              .space = space
                    }
                } );
            }
            // One polyline is ONE shape however many points it carries, so a
            // coarser batch costs resolution rather than round trips.
            auto added = overlay->add( grab::overlay::Shape{
                .geometry = grab::overlay::Path{ .commands = std::move( commands ) },
                .stroke =
                    grab::overlay::StrokeStyle{
                                                .color    = amber,
                                                .width_px = trail_stroke_px
                    },
                .fill     = std::nullopt,
                .lifetime = grab::overlay::Persistent{},
                .band     = grab::overlay::Band::Trail,
                .z        = 2,
            } );
            // Deliberately no flush inside the trajectory: that is a second
            // round trip per stroke, and the frame clock is already presenting.
            if( added.has_value() )
            {
                trail_ids.push_back( *added );
            }
            const motion::Vec2 tail = pending.back();
            pending.clear();
            pending.push_back( tail );
        };

        for( std::size_t step = 0U; step < movement.samples_.size(); ++step )
        {
            const motion::Sample& sample = movement.samples_[step];
            std::this_thread::sleep_until( deadline_of( sample ) );
            ( void )( *input ).move( static_cast<std::int16_t>( sample.x_ ),
                                     static_cast<std::int16_t>( sample.y_ ) );
            if( !options.trail )
            {
                continue;
            }
            // Sample carries integer device coordinates; the trail is drawn in
            // the same pixels the cursor was actually placed at, not in the
            // pre-rounding trajectory, so the stroke and the motion agree.
            pending.push_back( motion::Vec2{
                .x_ = static_cast<double>( sample.x_ ),
                .y_ = static_cast<double>( sample.y_ )
            } );
            const bool last = step + 1U == movement.samples_.size();
            if( last ||
                ( deadline_of( movement.samples_[step + 1U] ) -
                  std::chrono::steady_clock::now() ) > trail_slack )
            {
                stroke();
            }
        }
        std::cout << "  approach  " << movement.samples_.size() << " samples\n";
        if( options.trail )
        {
            // Persistent shapes are only guaranteed on screen once flushed, and
            // this is the one place the trail must be certain to be visible:
            // the frame the click-through claim is read from.
            ( void )overlay->flush();
            std::cout << "  trail     " << trail_ids.size() << " strokes on screen\n";
            // The claim, made visible: overlay pixels lying OVER the button,
            // with the press below about to reach the page anyway.
            const auto during = ( *screen ).display();
            if( during.has_value() )
            {
                pixel::write_ppm( *during, options.out / "02-approach.ppm" );
            }
        }

        // Where the press ACTUALLY happens, read from the X server. What this
        // program believes about the cursor proves nothing.
        const auto at_press = ( *input ).position();
        // Against the button's LIVE screen rect. The authored rect is in page
        // coordinates and the a11y rect in screen coordinates; they differ by
        // the browser chrome, so the authored numbers are checked as a size
        // below rather than as a position here.
        const bool inside = at_press.has_value() &&
                            live->rect_.contains( static_cast<double>( at_press->x ),
                                                  static_cast<double>( at_press->y ) );
        seen.push_back( stage::Observation{
            .observe_ = stage::Observe::CursorPosition,
            .subject_ = subject,
            .value_   = inside ? "inside" : "outside"
        } );
        seen.push_back( stage::Observation{
            .observe_ = stage::Observe::A11yBounds,
            .subject_ = subject,
            .value_   = std::to_string( static_cast<int>( live->rect_.w_ ) ) +
                        "x" +
                        std::to_string( static_cast<int>( live->rect_.h_ ) )
        } );
        if( at_press.has_value() )
        {
            std::cout << "  press     (" << at_press->x << "," << at_press->y << ") "
                      << ( inside ? "inside" : "OUTSIDE" ) << " the button\n";
        }

        // press/release rather than click(), so the hold is a sampled human
        // duration instead of however long two XTEST requests happen to take.
        const double hold_s = std::exp( rng.normal( std::log( 0.080 ), 0.35 ) );
        ( void )( *input ).press();
        std::this_thread::sleep_for( std::chrono::duration<double>( hold_s ) );
        ( void )( *input ).release();

        // The trail has now done its job -- it was over the button for the
        // whole press. Retire it before the observers read, so the a11y name,
        // the button's mean colour and the post-click frame are all measuring
        // the PAGE. Leaving it up would let the overlay answer a question
        // asked about what is underneath it.
        if( options.trail && overlay != nullptr && !trail_ids.empty() )
        {
            for( const grab::overlay::ShapeId id : trail_ids )
            {
                ( void )overlay->remove( id );
            }
            ( void )overlay->flush();
        }

        std::this_thread::sleep_for( std::chrono::milliseconds{ 50 } );

        // Drain button events and check for correlated DOWN+UP pair inside button
        std::string button_click_observed = "not observed";
        if( button_subscription.has_value() && live.has_value() )
        {
            std::optional<grab::SpacePoint> down_pos;
            std::optional<grab::SpacePoint> up_pos;

            while( auto btn_event = button_subscription->try_pop() )
            {
                const auto* mb = std::get_if<grab::MouseButton>( &btn_event->payload );
                if( mb == nullptr )
                {
                    continue;
                }

                if( mb->button != primary_button )
                {
                    continue;
                }

                if( btn_event->kind == grab::EventKind::MouseButtonDown )
                {
                    if( mb->position.has_value() )
                    {
                        down_pos = mb->position;
                    }
                }
                else if( btn_event->kind == grab::EventKind::MouseButtonUp )
                {
                    if( mb->position.has_value() )
                    {
                        up_pos = mb->position;
                    }
                }
            }

            // Check if we have both DOWN and UP with positions inside the rect
            if( down_pos.has_value() && up_pos.has_value() )
            {
                if( live->rect_.contains( down_pos->x, down_pos->y ) &&
                    live->rect_.contains( up_pos->x, up_pos->y ) )
                {
                    button_click_observed = "clicked";
                }
            }
        }
        seen.push_back( stage::Observation{
            .observe_ = stage::Observe::ButtonClick,
            .subject_ = subject,
            .value_   = button_click_observed
        } );
        std::cout << "  hold      " << static_cast<int>( hold_s * 1000.0 ) << " ms\n";

        std::this_thread::sleep_for( std::chrono::milliseconds{ react_ms } );
        goal( green );

        // ── 6. OBSERVE ──────────────────────────────────────────────────────
        std::string a11y_after = "(unresolved)";
        for( int attempt = 0; attempt < poll_tries; ++attempt )
        {
            if( auto again = resolve_button( **session ); again.has_value() )
            {
                a11y_after = again->name_;
                if( a11y_after == clicked_label )
                {
                    break;
                }
            }
            std::this_thread::sleep_for( std::chrono::milliseconds{ poll_ms } );
        }
        seen.push_back( stage::Observation{
            .observe_ = stage::Observe::A11yName,
            .subject_ = subject,
            .value_   = a11y_after
        } );
        std::cout << "  a11y      name=\"" << a11y_after << "\"\n";

        std::this_thread::sleep_for( std::chrono::milliseconds{ settle_ms } );
        auto        after   = ( *screen ).display();
        std::string flipped = "unreadable";
        if( before.has_value() && after.has_value() )
        {
            const auto was = pixel::mean_colour( *before, live->rect_ );
            const auto now = pixel::mean_colour( *after, live->rect_ );
            if( was.has_value() && now.has_value() )
            {
                const double moved = pixel::distance( *was, *now );
                flipped            = moved >= colour_match ? "changed" : "unchanged";
                std::cout << "  pixel     mean colour moved "
                          << static_cast<int>( moved )
                          << " (>= " << static_cast<int>( colour_match )
                          << " counts as changed)\n";
            }
        }
        seen.push_back( stage::Observation{
            .observe_ = stage::Observe::PixelColour,
            .subject_ = subject,
            .value_   = flipped
        } );

        if( after.has_value() )
        {
            pixel::write_ppm( *after, options.out / "03-after.ppm" );
        }

        // ── 7. SCORE ────────────────────────────────────────────────────────
        const stage::Scorecard card = stage::evaluate( scene, seen );
        std::cout << "\nSCORE\n";
        for( const stage::Assertion& entry : card.entries() )
        {
            std::cout << "  " << ( entry.pass_ ? "pass" : "FAIL" ) << "  ["
                      << entry.observer_ << "] " << entry.name_ << "  expected "
                      << entry.expected_ << ", got " << entry.actual_ << '\n';
        }
        {
            std::ofstream out( options.out / "scorecard.json" );
            out << "{\n  \"rung\": \"" << card.rung() << "\",\n  \"checks\": [\n";
            for( std::size_t index = 0U; index < card.entries().size(); ++index )
            {
                const stage::Assertion& entry = card.entries()[index];
                out << "    { \"observer\": \"" << entry.observer_ << "\", \"name\": \""
                    << entry.name_ << "\", \"expected\": \"" << entry.expected_
                    << "\", \"actual\": \"" << entry.actual_
                    << "\", \"pass\": " << ( entry.pass_ ? "true" : "false" ) << " }"
                    << ( index + 1U < card.entries().size() ? "," : "" ) << '\n';
            }
            out << "  ]\n}\n";
        }
        std::cout << "\nRESULT " << ( card.pass() ? "PASS" : "FAIL" ) << "  ("
                  << card.passed() << "/" << card.entries().size() << " checks)\n";
        exit_code = card.pass() ? 0 : 1;

        ( void )( *session )->stop_observation();
    }

    if( options.keep )
    {
        std::cout << "\n--keep: session left up on " << options.display << '\n';
        return exit_code;
    }
    host.stop();
    return exit_code;
}
