// ┌──────────────────────────────────────────────────────────────────────────┐
// │  stage_scroll_tour — rung 9, the long form: a scroll JOURNEY at          │
// │  variable speed, with a stop at every landmark.                          │
// │                                                                          │
// │  ONE page, five screenfuls tall, three buttons at three depths:          │
// │                                                                          │
// │    leg 1  DOWN, slowly       — one notch at a time, reading pace — to    │
// │           FIRST near the bottom of the page. Click it.                   │
// │    leg 2  UP, at walking pace — back to SECOND, midway up. Click it.     │
// │    leg 3  DOWN again, ramping — one, two, three notches and back down    │
// │           the ramp — to THIRD. Click it.                                 │
// │                                                                          │
// │  Each leg's NECESSITY is asserted before it runs: the target must be     │
// │  off-screen in the stated direction (below the window for the down       │
// │  legs, fully above it for the up leg) — a tour whose stops were all      │
// │  visible from the start would prove nothing about scrolling. Wheel       │
// │  distance per notch is a browser setting, so every leg re-reads the      │
// │  target's LIVE rect between bursts and self-corrects if a burst          │
// │  overshoots. The X server's own event stream must carry wheel events    │
// │  in BOTH directions by the end.                                          │
// │                                                                          │
// │    stage_scroll_tour                 headless, on a display it creates   │
// │    stage_scroll_tour --session       the display you are already on     │
// │    stage_scroll_tour --trail         approaches drawn on the overlay     │
// │    stage_scroll_tour --trail --watch same, in a Xephyr window           │
// │    stage_scroll_tour --keep          leave the session up afterwards     │
// └──────────────────────────────────────────────────────────────────────────┘

#include "support/host.hpp"
#include "support/motion/noise.hpp"
#include "support/overlay_align.hpp"
#include "support/motion/trajectory.hpp"
#include "support/pixel.hpp"
#include "support/stage/assert.hpp"
#include "support/stage/scene.hpp"
#include "support/surface.hpp"

#include <array>
#include <chrono>
#include <cmath>
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
#include <initializer_list>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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

    constexpr int           viewport_w = 1'000;
    constexpr int           viewport_h = 700;
    constexpr int           document_h = 3'600;    // five screenfuls

    // The three stops, in PAGE coordinates. The tour visits them in the
    // order FIRST (bottom), SECOND (midway, above FIRST), THIRD (between
    // the two) — down, up, down again.
    constexpr int           button_w = 400;
    constexpr int           button_h = 160;
    constexpr int           button_x = 300;
    constexpr int           first_y  = 3'000;
    constexpr int           second_y = 1'500;
    constexpr int           third_y  = 2'400;

    struct Stop
    {
            const char* subject_;
            const char* idle_label_;
            const char* done_label_;
            const char* idle_fill_;
            int         page_y_;
    };

    // Distinct idle fills, one shared done colour: "which button is this"
    // and "has it been clicked" are different questions and read on
    // different channels.
    constexpr const char* done_fill = "#2a9d3a";
    constexpr std::array<Stop, 3U> stops{
        Stop{ .subject_    = "btn_first",
              .idle_label_ = "FIRST",
              .done_label_ = "FIRST DONE",
              .idle_fill_  = "#1d4e89",
              .page_y_     = first_y },
        Stop{ .subject_    = "btn_second",
              .idle_label_ = "SECOND",
              .done_label_ = "SECOND DONE",
              .idle_fill_  = "#5b3a8f",
              .page_y_     = second_y },
        Stop{ .subject_    = "btn_third",
              .idle_label_ = "THIRD",
              .done_label_ = "THIRD DONE",
              .idle_fill_  = "#0f6b6b",
              .page_y_     = third_y },
    };

    constexpr const char*   title_marker = "Stage Scroll Tour";

    constexpr std::uint32_t wheel_up   = 4U;    // X11 wheel-up button code
    constexpr std::uint32_t wheel_down = 5U;    // X11 wheel-down button code

    constexpr const char*   wheel_subject   = "wheel";
    constexpr const char*   presses_subject = "presses";
    constexpr const char*   buttons_subject = "buttons";

    constexpr grab::overlay::Color cyan{ .r = 0U, .g = 217U, .b = 255U, .a = 242U };
    constexpr grab::overlay::Color amber{ .r = 255U, .g = 184U, .b = 26U, .a = 242U };

    constexpr double               stroke_px       = 3.0;
    constexpr double               trail_stroke_px = 2.0;
    constexpr auto                 trail_slack = std::chrono::milliseconds{ 8 };

    // Wheel park, as fractions of the live window frame (see stage_scroll).
    constexpr double        park_fraction_x = 0.72;
    constexpr double        park_fraction_y = 0.66;

    // ── The three paces ─────────────────────────────────────────────────────
    //
    // A pace is a repeating pattern of notches-per-burst plus the settle
    // between bursts. The tour's whole point is that these DIFFER: leg 1
    // crawls, leg 2 walks, leg 3 ramps up and back down. Distance per notch
    // is a browser setting, so a pace shapes the RHYTHM and the live rect
    // decides when a leg is over.
    struct Pace
    {
            const char*          name_;
            std::span<const int> pattern_;
            int                  settle_ms_;
    };

    constexpr std::array<int, 1U> slow_pattern{ 1 };
    constexpr std::array<int, 2U> walk_pattern{ 2, 1 };
    constexpr std::array<int, 6U> ramp_pattern{ 1, 2, 3, 3, 2, 1 };

    constexpr Pace slow_pace{ .name_      = "slow",
                              .pattern_   = slow_pattern,
                              .settle_ms_ = 350 };
    constexpr Pace walk_pace{ .name_      = "walk",
                              .pattern_   = walk_pattern,
                              .settle_ms_ = 220 };
    constexpr Pace ramp_pace{ .name_      = "ramp",
                              .pattern_   = ramp_pattern,
                              .settle_ms_ = 180 };

    constexpr int           max_leg_rounds   = 80;
    constexpr double        visible_margin   = 8.0;
    // A leg only ends when its target reads fully inside across TWO reads
    // this far apart — the settled rect is the one the click will aim at.
    constexpr int           stable_settle_ms = 300;

    constexpr std::uint64_t seed        = 0X5'D1'DE'00'0AULL;
    constexpr int           settle_ms   = 700;
    constexpr int           announce_ms = 700;
    constexpr int           react_ms    = 800;
    constexpr int           poll_ms     = 200;
    constexpr int    poll_tries   = 150;    // 30 s: Firefox builds its a11y tree lazily
    constexpr double colour_match = 40.0;

    [[nodiscard]]
    std::string
    page_html()
    {
        const auto px = []( int value )
        {
            return std::to_string( value ) + "px";
        };
        constexpr int mark_first  = 100;
        constexpr int mark_step   = 400;
        constexpr int mark_bottom = 200;
        std::string   sections;
        for( int top = mark_first; top < document_h - mark_bottom; top += mark_step )
        {
            sections += "<div class=\"mark\" style=\"top:" + px( top ) + "\">page y " +
                        std::to_string( top ) + "</div>\n";
        }
        std::string buttons;
        std::string script = "<script>\n";
        for( const Stop& stop : stops )
        {
            buttons += "<button id=\"" + std::string{ stop.subject_ } +
                       "\" aria-label=\"" + stop.idle_label_ +
                       "\" style=\"position:absolute;box-sizing:border-box;left:" +
                       px( button_x ) + ";top:" + px( stop.page_y_ ) +
                       ";width:" + px( button_w ) + ";height:" + px( button_h ) +
                       ";background:" + stop.idle_fill_ +
                       ";color:#fff;border:0;font:600 36px sans-serif;\">" +
                       stop.idle_label_ + "</button>\n";
            script += "  (function(){var b=document.getElementById('" +
                      std::string{ stop.subject_ } +
                      "');b.addEventListener('click',function(){"
                      "b.style.background='" +
                      done_fill + "';b.setAttribute('aria-label','" +
                      stop.done_label_ + "');b.textContent='" + stop.done_label_ +
                      "';});})();\n";
        }
        script += "</script>\n";
        return std::string{ "<!doctype html>\n"
                            "<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
                            "<title>Stage Scroll Tour — grab</title>\n"
                            "<style>\n"
                            "  html,body{margin:0;padding:0;background:#f4f4f4;}\n"
                            "  body{height:" } +
               px( document_h ) +
               ";position:relative;}\n"
               "  .mark{position:absolute;left:40px;width:400px;"
               "border-top:2px solid #b7c3d6;padding-top:4px;"
               "font:500 18px sans-serif;color:#66788f;}\n"
               "  #hint{position:absolute;left:300px;top:200px;"
               "font:600 44px sans-serif;color:#33415c;}\n"
               "</style></head><body>\n"
               // The viewport anchors: position:fixed strips at the content
               // area's exact top and bottom edges, invisible to the eye but
               // present in the a11y tree. Their rects ARE the content
               // bounds, live at any scroll — no window-frame arithmetic,
               // no chrome guess, no CSD-shadow surprise. pointer-events
               // none so they can never swallow a click.
               "<button aria-label=\"VIEWTOP\" tabindex=\"-1\" "
               "style=\"position:fixed;left:0;top:0;width:120px;height:3px;"
               "opacity:0.05;border:0;padding:0;pointer-events:none;"
               "background:#000;\"></button>\n"
               "<button aria-label=\"VIEWBOTTOM\" tabindex=\"-1\" "
               "style=\"position:fixed;left:0;bottom:0;width:120px;height:3px;"
               "opacity:0.05;border:0;padding:0;pointer-events:none;"
               "background:#000;\"></button>\n"
               "<div id=\"hint\">THE TOUR &#8595; &#8593; &#8595;</div>\n" +
               sections + buttons + script + "</body></html>\n";
    }

    [[nodiscard]]
    stage::Scene
    build_scene()
    {
        stage::Scene scene;
        scene.id_ = "scroll-tour";
        scene.pages_.push_back( stage::ScenePage{ .name_   = "tour",
                                                  .html_   = page_html(),
                                                  .marker_ = title_marker } );
        scene.viewport_ = stage::ViewportSpec{ .viewport_w_ = viewport_w,
                                               .viewport_h_ = viewport_h,
                                               .document_h_ = document_h };
        scene.frames_   = { "01-top", "02-first", "03-second", "04-third" };

        // DECLARED BEFORE THE ACT.
        const auto expect = [&]( std::string    name,
                                 stage::Observe observe,
                                 std::string    subj,
                                 std::string    value )
        {
            scene.expect_.push_back( stage::Expectation{ .name_    = std::move( name ),
                                                         .observe_ = observe,
                                                         .subject_ = std::move( subj ),
                                                         .value_   = std::move( value ),
                                                         .tolerance_ = 0.0,
                                                         .low_       = 0.0,
                                                         .high_      = 0.0,
                                                         .ranged_    = false } );
        };
        expect( "overlay_is_live", stage::Observe::Capability, "overlay", "live" );
        // Each leg's necessity: the target is off-screen in the stated
        // direction BEFORE its leg runs.
        expect( "first_starts_below",
                stage::Observe::A11yBounds,
                "leg1",
                "below" );
        expect( "second_starts_above",
                stage::Observe::A11yBounds,
                "leg2",
                "above" );
        expect( "third_starts_below",
                stage::Observe::A11yBounds,
                "leg3",
                "below" );
        // Each stop clicked, on the a11y channel.
        for( const Stop& stop : stops )
        {
            expect( std::string{ stop.subject_ } + "_clicked",
                    stage::Observe::A11yName,
                    stop.subject_,
                    stop.done_label_ );
        }
        // The journey, on the device channel: both wheel directions seen.
        expect( "wheel_both_directions",
                stage::Observe::ButtonClick,
                wheel_subject,
                "both" );
        // Every press inside its target, read back from the X server.
        expect( "presses_inside",
                stage::Observe::CursorPosition,
                presses_subject,
                "inside" );
        // Every button's pixels flipped between its before and after frame.
        expect( "pixels_flipped_all",
                stage::Observe::PixelColour,
                buttons_subject,
                "changed" );
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

    struct Live
    {
            std::string             name_;
            view::ViewRect          rect_{};
            grab::CoordinateSpaceId space_{};
    };

    [[nodiscard]]
    std::optional<Live>
    resolve_named( grab::Session&                          session,
                   std::initializer_list<std::string_view> names )
    {
        if( auto synced = session.resync(); !synced.has_value() )
        {
            return std::nullopt;
        }
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
            for( const std::string_view wanted : names )
            {
                if( info.name == wanted )
                {
                    return Live{
                        .name_  = info.name,
                        .rect_  = view::ViewRect{ .x_ = info.bounds.x,
                                                  .y_ = info.bounds.y,
                                                  .w_ = info.bounds.w,
                                                  .h_ = info.bounds.h },
                        .space_ = info.bounds.space,
                    };
                }
            }
        }
        return std::nullopt;
    }

    // `content_top` is the ABSOLUTE screen y where the page's content area
    // begins — the window's top edge plus the measured chrome (tab strip,
    // URL bar). It matters because a11y rects do NOT clip at the chrome: a
    // button half-scrolled under the URL bar still reports its layout
    // position, so a window-frame check admits it as "fully inside" while
    // its centre — the click target — sits in the URL bar. An up-leg parks
    // its target at the top edge, which is exactly where that bites.
    // `content_bottom` matters as much as `content_top`: the window FRAME's
    // bottom edge is the wrong bound (under mutter it includes the CSD
    // shadow), and a target parked with its bottom clipped by the real
    // content edge is a trap — mousedown focuses it, the browser scrolls it
    // into view mid-click, and the release lands on a moved page: no click
    // ever fires. Both edges come from the page's own fixed anchors.
    [[nodiscard]]
    bool
    fully_inside( const view::ViewRect& rect,
                  const view::ViewRect& window,
                  double                content_top,
                  double                content_bottom,
                  double                screen_w,
                  double                screen_h )
    {
        return rect.y_ >= content_top + visible_margin &&
               ( rect.y_ + rect.h_ ) <= content_bottom - visible_margin &&
               rect.x_ >= window.x_ &&
               ( rect.x_ + rect.w_ ) <= window.x_ + window.w_ &&
               rect.y_ >= 0.0 && ( rect.y_ + rect.h_ ) <= screen_h &&
               rect.x_ >= 0.0 && ( rect.x_ + rect.w_ ) <= screen_w;
    }

    struct Options
    {
            std::string           display = ":69";
            std::filesystem::path out     = "stage-scroll-tour";
            std::string           host_display;
            bool                  attach = false;
            bool                  keep   = false;
            bool                  trail  = false;
    };

    void
    usage()
    {
        std::cout
            << "stage_scroll_tour — rung 9 long form: down, up, down, a click "
               "at every stop\n\n"
               "  --display :N   display to create (ignored with --session)\n"
               "  --out DIR      where the page, frames and scorecard land\n"
               "  --session      drive the display you are already on\n"
               "  --watch        show the nested display in a window here\n"
               "  --trail        draw the approaches on the overlay\n"
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
                std::cerr << "stage_scroll_tour: " << flag << ' ' << reason << "\n\n";
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
                    return bad( "requires a display, e.g. :69" );
                }
            }
            else if( flag == "--out" )
            {
                const std::string raw = value();
                if( missing || raw.empty() )
                {
                    return bad( "requires a directory" );
                }
                // Absolute from the start: the host points Firefox's HOME
                // here, and Firefox rejects a relative HOME.
                options.out = std::filesystem::absolute( raw );
            }
            else if( flag == "--session" )
            {
                options.attach = true;
                // NOLINTNEXTLINE(concurrency-mt-unsafe)
                const char* const ambient = std::getenv( "DISPLAY" );
                if( ambient == nullptr || ambient[0] == '\0' )
                {
                    std::cerr << "stage_scroll_tour: --session needs DISPLAY set\n";
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
        if( options.attach && !options.host_display.empty() )
        {
            std::cerr << "stage_scroll_tour: --watch and --session are mutually "
                         "exclusive\n\n";
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
    const std::filesystem::path page = options.out / "tour.html";
    {
        std::ofstream out( page );
        out << scene.pages_.front().html_;
    }
    std::cout << "AUTHOR\n  document  " << viewport_w << "x" << document_h
              << " (viewport " << viewport_w << "x" << viewport_h << ")\n";
    for( const Stop& stop : stops )
    {
        std::cout << "  " << stop.idle_label_ << std::string(
                         10U - std::string_view{ stop.idle_label_ }.size(), ' ' )
                  << "page y " << stop.page_y_ << '\n';
    }

    // ── 2. HOST ─────────────────────────────────────────────────────────────
    std::cout << "\nHOST\n";
    ladder::host::Host host{ options.display,
                             options.out,
                             std::to_string( viewport_w ) + "x" +
                                 std::to_string( viewport_h ),
                             options.host_display,
                             options.attach,
                             title_marker };
    if( !host.start( "file://" + std::filesystem::absolute( page ).string() ) )
    {
        std::cerr << "host did not come up\n";
        host.stop();
        return 1;
    }
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
        if( auto observing = ( *session )->start_observation(); !observing.has_value() )
        {
            std::cerr << "reactor: " << why( observing.error() ) << '\n';
        }

        std::optional<grab::Subscription> button_subscription;
        {
            grab::SubscriptionScope button_scope;
            button_scope.kinds = { grab::EventKind::MouseButtonDown,
                                   grab::EventKind::MouseButtonUp };
            auto btn_sub       = ( *session )->watch( button_scope );
            if( btn_sub.has_value() )
            {
                button_subscription = std::move( *btn_sub );
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
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::Capability,
                                            .subject_ = "overlay",
                                            .value_   = overlay_state } );
        std::cout << "  overlay   " << overlay_state << '\n';

        // ── 3. RESOLVE ──────────────────────────────────────────────────────
        std::optional<Live> probe;
        for( int attempt = 0; attempt < poll_tries && !probe.has_value(); ++attempt )
        {
            probe = resolve_named( **session, { stops[0].idle_label_ } );
            if( !probe.has_value() )
            {
                std::this_thread::sleep_for( std::chrono::milliseconds{ poll_ms } );
            }
        }
        if( !probe.has_value() )
        {
            std::cerr << "the buttons never appeared in the accessibility tree\n";
            host.stop();
            return 1;
        }

        const auto summary = host.browser_window();
        if( !summary.has_value() )
        {
            std::cerr << "the browser window disappeared from the window list\n";
            host.stop();
            return 1;
        }
        const view::ViewRect window_rect{
            .x_ = static_cast<double>( summary->bounds.x ),
            .y_ = static_cast<double>( summary->bounds.y ),
            .w_ = static_cast<double>( summary->bounds.width ),
            .h_ = static_cast<double>( summary->bounds.height ),
        };
        std::cout << "  window    (" << summary->bounds.x << "," << summary->bounds.y
                  << " " << summary->bounds.width << "x" << summary->bounds.height
                  << ")\n";

        auto   top_frame = ( *screen ).display();
        double screen_w  = window_rect.x_ + window_rect.w_;
        double screen_h  = window_rect.y_ + window_rect.h_;
        if( top_frame.has_value() )
        {
            pixel::write_ppm( *top_frame, options.out / "01-top.ppm" );
            screen_w = static_cast<double>( top_frame->width );
            screen_h = static_cast<double>( top_frame->height );
        }

        // The content area's edges, from the page's own fixed anchors: two
        // invisible position:fixed strips authored at top:0 and bottom:0,
        // whose a11y rects ARE the content bounds, live at any scroll. This
        // replaced two generations of guesswork — a chrome height inferred
        // at scroll 0 (which fixed the URL-bar click) and the window FRAME's
        // bottom edge (which, under mutter, includes the CSD shadow and
        // admitted a bottom-clipped target whose mid-click focus scroll
        // swallowed the click).
        const auto view_top = resolve_named( **session, { "VIEWTOP" } );
        const auto view_bottom = resolve_named( **session, { "VIEWBOTTOM" } );
        if( !view_top.has_value() || !view_bottom.has_value() )
        {
            std::cerr << "the viewport anchors never resolved — REFUSING to "
                         "scroll blind on this display\n";
            host.stop();
            return 1;
        }
        const double content_top    = view_top->rect_.y_ + view_top->rect_.h_;
        const double content_bottom = view_bottom->rect_.y_;
        std::cout << "  content   y " << static_cast<int>( content_top ) << " .. "
                  << static_cast<int>( content_bottom )
                  << " (from the page's fixed anchors)\n";

        // Where do overlay shapes ACTUALLY land? Measured over the page.
        const auto omap = ladder::view::align::measure(
            overlay, *screen, space,
            window_rect.x_ + ( window_rect.w_ * 0.55 ), content_top + 60.0 );

        // ── 4. THE TOUR ─────────────────────────────────────────────────────
        const auto park_x = static_cast<std::int16_t>(
            window_rect.x_ + ( window_rect.w_ * park_fraction_x )
        );
        const auto park_y = static_cast<std::int16_t>(
            window_rect.y_ + ( window_rect.h_ * park_fraction_y )
        );
        ( void )( *input ).move( park_x, park_y );
        std::this_thread::sleep_for( std::chrono::milliseconds{ settle_ms } );

        motion::Rng                         rng{ seed };
        std::vector<motion::Vec2>           pending;
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
                .point = grab::SpacePoint{ .x     = omap.x( pending.front().x_ ),
                                           .y     = omap.y( pending.front().y_ ),
                                           .space = space } } );
            for( std::size_t step = 1U; step < pending.size(); ++step )
            {
                commands.emplace_back( grab::overlay::LineTo{
                    .point = grab::SpacePoint{ .x     = omap.x( pending[step].x_ ),
                                               .y     = omap.y( pending[step].y_ ),
                                               .space = space } } );
            }
            auto added = overlay->add( grab::overlay::Shape{
                .geometry = grab::overlay::Path{ .commands = std::move( commands ) },
                .stroke   = grab::overlay::StrokeStyle{ .color    = amber,
                                                        .width_px = trail_stroke_px },
                .fill     = std::nullopt,
                .lifetime = grab::overlay::Persistent{},
                .band     = grab::overlay::Band::Trail,
                .z        = 2,
            } );
            if( added.has_value() )
            {
                trail_ids.push_back( *added );
            }
            const motion::Vec2 tail = pending.back();
            pending.clear();
            pending.push_back( tail );
        };
        const auto retire_trail = [&]()
        {
            if( overlay == nullptr || trail_ids.empty() )
            {
                return;
            }
            for( const grab::overlay::ShapeId id : trail_ids )
            {
                ( void )overlay->remove( id );
            }
            trail_ids.clear();
            pending.clear();
            ( void )overlay->flush();
        };

        // One scroll leg: burst notches at the pace's rhythm, re-reading the
        // live rect and choosing the direction from where the target IS —
        // which is also what makes an overshoot self-correcting.
        const auto bring_into_view =
            [&]( std::string_view label,
                 const Pace&      pace ) -> std::optional<Live>
        {
            std::optional<Live> target = resolve_named( **session, { label } );
            int                 rounds = 0;
            std::size_t         beat   = 0U;
            while( target.has_value() && rounds < max_leg_rounds )
            {
                if( fully_inside( target->rect_, window_rect, content_top,
                                  content_bottom, screen_w, screen_h ) )
                {
                    // The predicate held once — but the browser may still be
                    // settling the last burst. The rect the CLICK will aim at
                    // must be the settled one, so confirm it holds across a
                    // settle before believing it; a drifted target re-enters
                    // the loop and keeps scrolling.
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds{ stable_settle_ms }
                    );
                    target = resolve_named( **session, { label } );
                    if( target.has_value() &&
                        fully_inside( target->rect_, window_rect, content_top,
                                      content_bottom, screen_w, screen_h ) )
                    {
                        break;
                    }
                    continue;
                }
                const int notches = pace.pattern_[beat % pace.pattern_.size()];
                ++beat;
                const bool needs_down =
                    ( target->rect_.y_ + target->rect_.h_ ) >
                    content_bottom - visible_margin;
                ( void )( *input ).scroll( 0, needs_down ? notches : -notches );
                ++rounds;
                std::this_thread::sleep_for(
                    std::chrono::milliseconds{ pace.settle_ms_ }
                );
                target = resolve_named( **session, { label } );
            }
            std::cout << "  leg[" << pace.name_ << "] " << label << ": " << rounds
                      << " burst(s)";
            if( target.has_value() )
            {
                std::cout << ", now at (" << static_cast<int>( target->rect_.x_ )
                          << "," << static_cast<int>( target->rect_.y_ ) << ")";
            }
            std::cout << '\n';
            return target;
        };

        // Approach the (visible) target on a human trajectory and click it.
        // Returns whether the press point read back from the server was inside.
        const auto approach_and_click = [&]( const Live& target ) -> bool
        {
            if( overlay != nullptr )
            {
                ( void )overlay->add( grab::overlay::Shape{
                    .geometry = grab::overlay::Rect{
                        .bounds = grab::SpaceRect{ .x = omap.x( target.rect_.x_ ),
                                                   .y     = omap.y( target.rect_.y_ ),
                                                   .w     = target.rect_.w_ / omap.sx_,
                                                   .h     = target.rect_.h_ / omap.sy_,
                                                   .space = space } },
                    .stroke   = grab::overlay::StrokeStyle{ .color    = cyan,
                                                            .width_px = stroke_px },
                    .fill     = std::nullopt,
                    .lifetime =
                        grab::overlay::Ttl{
                            .duration = std::chrono::milliseconds{ 1'600 } },
                    .band = grab::overlay::Band::Annotation,
                    .z    = 0,
                } );
                ( void )overlay->flush();
            }
            std::this_thread::sleep_for( std::chrono::milliseconds{ announce_ms } );

            const auto   at = ( *input ).position();
            motion::Vec2 cursor{ at.has_value() ? static_cast<double>( at->x )
                                                : target.rect_.x_,
                                 at.has_value() ? static_cast<double>( at->y )
                                                : target.rect_.y_ };
            const motion::Rect aim{ target.rect_.x_,
                                    target.rect_.y_,
                                    target.rect_.w_,
                                    target.rect_.h_ };
            const auto         movement =
                motion::plan_move( rng, cursor, aim, motion::MotionConfig{} );
            const double lead =
                movement.samples_.empty() ? 0.0 : movement.samples_.front().t_s_;
            const auto begin       = std::chrono::steady_clock::now();
            const auto deadline_of = [&]( const motion::Sample& sample )
            {
                return begin +
                       std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                           std::chrono::duration<double>( sample.t_s_ - lead )
                       );
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
                pending.push_back(
                    motion::Vec2{ .x_ = static_cast<double>( sample.x_ ),
                                  .y_ = static_cast<double>( sample.y_ ) }
                );
                const bool final_sample = step + 1U == movement.samples_.size();
                if( final_sample ||
                    ( deadline_of( movement.samples_[step + 1U] ) -
                      std::chrono::steady_clock::now() ) > trail_slack )
                {
                    stroke();
                }
            }
            if( options.trail && overlay != nullptr )
            {
                ( void )overlay->flush();
            }

            const auto at_press = ( *input ).position();
            const bool inside   = at_press.has_value() &&
                                target.rect_.contains(
                                    static_cast<double>( at_press->x ),
                                    static_cast<double>( at_press->y ) );
            const double hold_s = std::exp( rng.normal( std::log( 0.080 ), 0.35 ) );
            ( void )( *input ).press();
            std::this_thread::sleep_for( std::chrono::duration<double>( hold_s ) );
            ( void )( *input ).release();
            retire_trail();
            return inside;
        };

        // The three legs. Before each: assert the target is off-screen in
        // the stated direction. After each click: wait, re-read the name,
        // capture the after frame, compare the pixels.
        const std::array<const char*, 3U> leg_subjects{ "leg1", "leg2", "leg3" };
        const std::array<bool, 3U>        leg_expect_below{ true, false, true };
        const std::array<Pace, 3U> leg_paces{ slow_pace, walk_pace, ramp_pace };
        const std::array<const char*, 3U> after_frames{ "02-first.ppm",
                                                        "03-second.ppm",
                                                        "04-third.ppm" };
        bool all_presses_inside = true;
        bool all_pixels_flipped = true;

        for( std::size_t leg = 0U; leg < stops.size(); ++leg )
        {
            const Stop& stop = stops[leg];

            // Necessity: where is the target BEFORE this leg scrolls?
            std::string direction = "on screen";
            if( auto before_leg = resolve_named( **session, { stop.idle_label_ } );
                before_leg.has_value() )
            {
                if( before_leg->rect_.y_ >= content_bottom )
                {
                    direction = "below";
                }
                else if( before_leg->rect_.y_ + before_leg->rect_.h_ <=
                         content_top )
                {
                    direction = "above";
                }
            }
            seen.push_back( stage::Observation{
                .observe_ = stage::Observe::A11yBounds,
                .subject_ = leg_subjects[leg],
                .value_   = direction } );
            std::cout << "  " << stop.idle_label_ << " starts " << direction
                      << " — expected "
                      << ( leg_expect_below[leg] ? "below" : "above" ) << '\n';

            const auto target = bring_into_view( stop.idle_label_,
                                                 leg_paces[leg] );
            if( !target.has_value() ||
                !fully_inside( target->rect_, window_rect, content_top,
                               content_bottom, screen_w, screen_h ) )
            {
                std::cerr << "leg " << leg + 1U << " never brought "
                          << stop.idle_label_ << " on screen\n";
                continue;
            }

            const auto before_click = ( *screen ).display();
            if( !approach_and_click( *target ) )
            {
                all_presses_inside = false;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds{ react_ms } );

            std::string name_after = "(unresolved)";
            for( int attempt = 0; attempt < poll_tries; ++attempt )
            {
                if( auto again = resolve_named(
                        **session, { stop.done_label_, stop.idle_label_ } );
                    again.has_value() )
                {
                    name_after = again->name_;
                    if( name_after == stop.done_label_ )
                    {
                        break;
                    }
                }
                std::this_thread::sleep_for( std::chrono::milliseconds{ poll_ms } );
            }
            seen.push_back( stage::Observation{ .observe_ = stage::Observe::A11yName,
                                                .subject_ = stop.subject_,
                                                .value_   = name_after } );

            const auto after_click = ( *screen ).display();
            if( after_click.has_value() )
            {
                pixel::write_ppm( *after_click, options.out / after_frames[leg] );
            }
            std::string flip = "unreadable";
            if( before_click.has_value() && after_click.has_value() )
            {
                const auto was = pixel::mean_colour( *before_click, target->rect_ );
                const auto now = pixel::mean_colour( *after_click, target->rect_ );
                if( was.has_value() && now.has_value() )
                {
                    const double moved = pixel::distance( *was, *now );
                    flip = moved >= colour_match ? "changed" : "unchanged";
                }
            }
            if( flip != "changed" )
            {
                all_pixels_flipped = false;
            }
            std::cout << "  " << stop.idle_label_ << " -> \"" << name_after
                      << "\", pixels " << flip << '\n';
        }

        seen.push_back( stage::Observation{ .observe_ = stage::Observe::CursorPosition,
                                            .subject_ = presses_subject,
                                            .value_ = all_presses_inside
                                                          ? "inside"
                                                          : "outside" } );
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::PixelColour,
                                            .subject_ = buttons_subject,
                                            .value_   = all_pixels_flipped
                                                            ? "changed"
                                                            : "unchanged" } );

        // The journey on the device channel: the X server must have seen
        // wheel events in BOTH directions.
        std::string wheel_seen = "not observed";
        if( button_subscription.has_value() )
        {
            int downs = 0;
            int ups   = 0;
            while( auto btn_event = button_subscription->try_pop() )
            {
                const auto* mb = std::get_if<grab::MouseButton>( &btn_event->payload );
                if( mb == nullptr ||
                    btn_event->kind != grab::EventKind::MouseButtonDown )
                {
                    continue;
                }
                if( mb->button == wheel_down )
                {
                    ++downs;
                }
                else if( mb->button == wheel_up )
                {
                    ++ups;
                }
            }
            if( downs > 0 && ups > 0 )
            {
                wheel_seen = "both";
            }
            else if( downs > 0 || ups > 0 )
            {
                wheel_seen = "one direction";
            }
            std::cout << "  events    " << downs << " wheel-down, " << ups
                      << " wheel-up notch(es)\n";
        }
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::ButtonClick,
                                            .subject_ = wheel_subject,
                                            .value_   = wheel_seen } );

        // ── 5. SCORE ────────────────────────────────────────────────────────
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
