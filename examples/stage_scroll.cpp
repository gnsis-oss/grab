// ┌──────────────────────────────────────────────────────────────────────────┐
// │  stage_scroll — rung 9 of the capability ladder: wheel · then click.     │
// │                                                                          │
// │  ONE page, four screenfuls tall. The button lives at the BOTTOM, below   │
// │  the fold: at start its accessibility rect sits beyond the screen's      │
// │  bottom edge, which is asserted — a run that could click without         │
// │  scrolling would be testing nothing. The pointer parks over the page,    │
// │  the wheel turns in bursts, and the button's LIVE rect is re-read        │
// │  between bursts until it is fully on screen. Wheel distance per notch    │
// │  is a browser setting, so no exact offset is ever asserted — the loop    │
// │  measures instead of assuming. Then: approach, click, verify.            │
// │                                                                          │
// │  Checked across channels that cannot cover for each other:              │
// │                                                                          │
// │    a11y    below the fold at start; fully visible after scrolling;       │
// │            the name flips BOTTOM -> CLICKED                              │
// │    pixel   the button's fill flips from one authored colour to another   │
// │    device  the X server's event stream carries the wheel notches AND     │
// │            a press/release pair inside the button's final rect           │
// │                                                                          │
// │    stage_scroll                 headless, on a display it creates        │
// │    stage_scroll --session       the display you are already on          │
// │    stage_scroll --trail         the approach drawn on the overlay        │
// │    stage_scroll --trail --watch same, in a Xephyr WINDOW you can watch   │
// │    stage_scroll --keep          leave the session up afterwards          │
// └──────────────────────────────────────────────────────────────────────────┘

#include "support/host.hpp"
#include "support/motion/noise.hpp"
#include "support/motion/trajectory.hpp"
#include "support/pixel.hpp"
#include "support/stage/assert.hpp"
#include "support/stage/scene.hpp"
#include "support/surface.hpp"

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
    constexpr int           document_h = 2'800;    // four screenfuls

    // The button, in PAGE coordinates — deliberately in the last screenful.
    constexpr int           button_x = 300;
    constexpr int           button_y = 2'400;
    constexpr int           button_w = 400;
    constexpr int           button_h = 160;

    constexpr const char*   idle_label    = "BOTTOM";
    constexpr const char*   clicked_label = "CLICKED";
    constexpr const char*   idle_fill     = "#1d4e89";
    constexpr const char*   clicked_fill  = "#c1121f";
    constexpr const char*   title_marker  = "Stage Scroll";

    constexpr std::uint32_t primary_button = 1U;    // X11 primary pointer button
    constexpr std::uint32_t wheel_down     = 5U;    // X11 wheel-down button code

    constexpr const char*   subject       = "btn_bottom";
    constexpr const char*   start_subject = "start";
    constexpr const char*   view_subject  = "scrolled";
    constexpr const char*   wheel_subject = "wheel";

    constexpr grab::overlay::Color cyan{ .r = 0U, .g = 217U, .b = 255U, .a = 242U };
    constexpr grab::overlay::Color amber{ .r = 255U, .g = 184U, .b = 26U, .a = 242U };

    constexpr double               stroke_px       = 3.0;
    constexpr double               trail_stroke_px = 2.0;
    constexpr auto                 trail_slack = std::chrono::milliseconds{ 8 };

    // Where the pointer parks to scroll: over the page, clear of the column
    // the button will rise through, so the approach still has a visible sweep.
    constexpr std::int16_t  scroll_park_x = 850;
    constexpr std::int16_t  scroll_park_y = 400;

    // Wheel bursts: a few notches, then re-read the live rect. The distance
    // one notch travels is a browser setting (ladder §5.4), so the loop is
    // bounded by rounds, never by an assumed pixels-per-notch.
    constexpr int           notches_per_round = 3;
    constexpr int           max_scroll_rounds = 40;
    constexpr int           scroll_settle_ms  = 180;
    // Fully visible means the whole rect inside the screen with this margin.
    constexpr double        visible_margin = 8.0;

    constexpr std::uint64_t seed        = 0X5'D1'DE'00'09ULL;
    constexpr int           settle_ms   = 700;
    constexpr int           announce_ms = 900;
    constexpr int           react_ms    = 900;
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
        // Section markers down the page, so a capture shows WHERE in the page
        // the viewport is — a scroll that silently failed would otherwise
        // produce frames that all look alike.
        constexpr int mark_first  = 100;
        constexpr int mark_step   = 400;
        constexpr int mark_bottom = 200;    // keep clear of the button
        std::string   sections;
        for( int top = mark_first; top < document_h - mark_bottom; top += mark_step )
        {
            sections += "<div class=\"mark\" style=\"top:" + px( top ) + "\">page y " +
                        std::to_string( top ) + "</div>\n";
        }
        return std::string{ "<!doctype html>\n"
                            "<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
                            "<title>Stage Scroll — grab</title>\n"
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
               "  #btn_bottom{position:absolute;box-sizing:border-box;"
               "left:" +
               px( button_x ) + ";top:" + px( button_y ) + ";width:" + px( button_w ) +
               ";height:" + px( button_h ) +
               ";background:" + idle_fill +
               ";color:#fff;border:0;font:600 40px sans-serif;}\n"
               "</style></head><body>\n"
               "<div id=\"hint\">SCROLL DOWN &#8595;</div>\n" +
               sections +
               "<button id=\"btn_bottom\" aria-label=\"" + idle_label + "\">" +
               idle_label +
               "</button>\n"
               "<script>\n"
               "  var b=document.getElementById('btn_bottom');\n"
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
        scene.id_ = "scroll";
        scene.pages_.push_back( stage::ScenePage{ .name_   = "scroll",
                                                  .html_   = page_html(),
                                                  .marker_ = title_marker } );
        scene.viewport_ = stage::ViewportSpec{ .viewport_w_ = viewport_w,
                                               .viewport_h_ = viewport_h,
                                               .document_h_ = document_h };
        scene.frames_   = { "01-top", "02-scrolled", "03-after" };

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
        expect( "button_below_fold_at_start",
                stage::Observe::A11yBounds,
                start_subject,
                "below" );
        expect( "scroll_brought_button_on_screen",
                stage::Observe::A11yBounds,
                view_subject,
                "visible" );
        expect( "wheel_notches_observed",
                stage::Observe::ButtonClick,
                wheel_subject,
                "scrolled" );
        expect( "press_inside_button",
                stage::Observe::CursorPosition,
                subject,
                "inside" );
        expect( "button_click_observed",
                stage::Observe::ButtonClick,
                subject,
                "clicked" );
        expect( "a11y_reports_clicked",
                stage::Observe::A11yName,
                subject,
                clicked_label );
        expect( "pixels_flipped", stage::Observe::PixelColour, subject, "changed" );
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

    [[nodiscard]]
    bool
    fully_on_screen( const view::ViewRect& rect )
    {
        return rect.y_ >= visible_margin &&
               ( rect.y_ + rect.h_ ) <=
                   static_cast<double>( viewport_h ) - visible_margin &&
               rect.x_ >= 0.0 &&
               ( rect.x_ + rect.w_ ) <= static_cast<double>( viewport_w );
    }

    struct Options
    {
            std::string           display = ":70";
            std::filesystem::path out     = "stage-scroll";
            std::string           host_display;
            bool                  attach = false;
            bool                  keep   = false;
            bool                  trail  = false;
    };

    void
    usage()
    {
        std::cout << "stage_scroll — rung 9: scroll a long page, then click\n\n"
                     "  --display :N   display to create (ignored with --session)\n"
                     "  --out DIR      where the page, frames and scorecard land\n"
                     "  --session      drive the display you are already on\n"
                     "  --watch        show the nested display in a window here\n"
                     "  --trail        draw the approach on the overlay\n"
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
                std::cerr << "stage_scroll: " << flag << ' ' << reason << "\n\n";
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
                    return bad( "requires a display, e.g. :70" );
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
                    std::cerr << "stage_scroll: --session needs DISPLAY set\n";
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
            std::cerr
                << "stage_scroll: --watch and --session are mutually exclusive\n\n";
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
    const std::filesystem::path page = options.out / "scroll.html";
    {
        std::ofstream out( page );
        out << scene.pages_.front().html_;
    }
    std::cout << "AUTHOR\n  document  " << viewport_w << "x" << document_h
              << " (viewport " << viewport_w << "x" << viewport_h
              << ")\n  button    (" << button_x << "," << button_y << " " << button_w
              << "x" << button_h << ", page coords) -> " << page << '\n';

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

        // The X server's own record: wheel notches are button 4/5 presses, so
        // one subscription witnesses both the scrolling and the click.
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

        // ── 3. RESOLVE — the button must start BELOW the fold ───────────────
        std::optional<Live> live;
        for( int attempt = 0; attempt < poll_tries && !live.has_value(); ++attempt )
        {
            live = resolve_named( **session, { idle_label } );
            if( !live.has_value() )
            {
                std::this_thread::sleep_for( std::chrono::milliseconds{ poll_ms } );
            }
        }
        if( !live.has_value() )
        {
            std::cerr << "the button never appeared in the accessibility tree\n";
            host.stop();
            return 1;
        }
        const bool below = live->rect_.y_ >= static_cast<double>( viewport_h );
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::A11yBounds,
                                            .subject_ = start_subject,
                                            .value_ = below ? "below" : "on screen" } );
        std::cout << "  button    a11y (" << static_cast<int>( live->rect_.x_ ) << ","
                  << static_cast<int>( live->rect_.y_ ) << " "
                  << static_cast<int>( live->rect_.w_ ) << "x"
                  << static_cast<int>( live->rect_.h_ ) << ") name=\"" << live->name_
                  << "\" — " << ( below ? "below the fold" : "ALREADY ON SCREEN" )
                  << '\n';

        auto top_frame = ( *screen ).display();
        if( top_frame.has_value() )
        {
            pixel::write_ppm( *top_frame, options.out / "01-top.ppm" );
        }

        // ── 4. SCROLL — measured, never assumed ─────────────────────────────
        // The wheel goes to the window under the pointer, so park over the
        // page first. Each round turns a few notches, waits for the browser
        // to settle, and re-reads the LIVE rect; pixels-per-notch is a
        // browser setting nothing here relies on.
        ( void )( *input ).move( scroll_park_x, scroll_park_y );
        std::this_thread::sleep_for( std::chrono::milliseconds{ settle_ms } );

        int rounds = 0;
        while( rounds < max_scroll_rounds && !fully_on_screen( live->rect_ ) )
        {
            ( void )( *input ).scroll( 0, notches_per_round );
            ++rounds;
            std::this_thread::sleep_for(
                std::chrono::milliseconds{ scroll_settle_ms }
            );
            if( auto again = resolve_named( **session, { idle_label } );
                again.has_value() )
            {
                live = again;
            }
        }
        const bool visible = fully_on_screen( live->rect_ );
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::A11yBounds,
                                            .subject_ = view_subject,
                                            .value_   = visible ? "visible"
                                                                : "still hidden" } );
        std::cout << "  scroll    " << rounds << " round(s) of " << notches_per_round
                  << " notch(es); button now at ("
                  << static_cast<int>( live->rect_.x_ ) << ","
                  << static_cast<int>( live->rect_.y_ ) << ") — "
                  << ( visible ? "fully on screen" : "STILL HIDDEN" ) << '\n';
        if( !visible )
        {
            std::cerr << "scrolling never brought the button on screen\n";
        }

        // ── 5. DRAW · APPROACH · CLICK ──────────────────────────────────────
        const auto goal = [&]( const grab::overlay::Color& colour )
        {
            if( overlay == nullptr )
            {
                return;
            }
            ( void )overlay->add( grab::overlay::Shape{
                .geometry =
                    grab::overlay::Rect{
                                        .bounds =
                            grab::SpaceRect{ .x     = live->rect_.x_,
                                             .y     = live->rect_.y_,
                                             .w     = live->rect_.w_,
                                             .h     = live->rect_.h_,
                                             .space = space } },
                .stroke   = grab::overlay::StrokeStyle{ .color    = colour,
                                                        .width_px = stroke_px },
                .fill     = std::nullopt,
                .lifetime = grab::overlay::Persistent{},
                .band     = grab::overlay::Band::Annotation,
                .z        = 0,
            } );
            ( void )overlay->flush();
        };
        goal( cyan );
        std::this_thread::sleep_for( std::chrono::milliseconds{ announce_ms } );

        // The pixel baseline: the button visible, still idle-coloured.
        auto before = ( *screen ).display();
        if( before.has_value() )
        {
            pixel::write_ppm( *before, options.out / "02-scrolled.ppm" );
        }

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
                .point = grab::SpacePoint{ .x     = pending.front().x_,
                                           .y     = pending.front().y_,
                                           .space = space } } );
            for( std::size_t step = 1U; step < pending.size(); ++step )
            {
                commands.emplace_back( grab::overlay::LineTo{
                    .point = grab::SpacePoint{ .x     = pending[step].x_,
                                               .y     = pending[step].y_,
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

        const auto   start = ( *input ).position();
        motion::Vec2 cursor{ start.has_value() ? static_cast<double>( start->x )
                                               : static_cast<double>( scroll_park_x ),
                             start.has_value() ? static_cast<double>( start->y )
                                               : static_cast<double>( scroll_park_y ) };
        const motion::Rect aim{ live->rect_.x_,
                                live->rect_.y_,
                                live->rect_.w_,
                                live->rect_.h_ };
        const auto movement = motion::plan_move( rng, cursor, aim,
                                                 motion::MotionConfig{} );
        {
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
        }
        std::cout << "  approach  " << movement.samples_.size() << " samples\n";
        if( options.trail && overlay != nullptr )
        {
            ( void )overlay->flush();
        }

        const auto at_press     = ( *input ).position();
        const bool press_inside = at_press.has_value() &&
                                  live->rect_.contains(
                                      static_cast<double>( at_press->x ),
                                      static_cast<double>( at_press->y ) );
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::CursorPosition,
                                            .subject_ = subject,
                                            .value_ = press_inside ? "inside"
                                                                   : "outside" } );
        if( at_press.has_value() )
        {
            std::cout << "  press     (" << at_press->x << "," << at_press->y << ") "
                      << ( press_inside ? "inside" : "OUTSIDE" ) << " the button\n";
        }

        const double hold_s = std::exp( rng.normal( std::log( 0.080 ), 0.35 ) );
        ( void )( *input ).press();
        std::this_thread::sleep_for( std::chrono::duration<double>( hold_s ) );
        ( void )( *input ).release();

        if( options.trail && overlay != nullptr && !trail_ids.empty() )
        {
            for( const grab::overlay::ShapeId id : trail_ids )
            {
                ( void )overlay->remove( id );
            }
            ( void )overlay->flush();
        }
        std::this_thread::sleep_for( std::chrono::milliseconds{ 50 } );

        // Drain the event stream once: wheel notches AND the click pair.
        std::string wheel_seen = "not observed";
        std::string click_seen = "not observed";
        if( button_subscription.has_value() )
        {
            int                             wheel_downs = 0;
            std::optional<grab::SpacePoint> down_pos;
            std::optional<grab::SpacePoint> up_pos;
            while( auto btn_event = button_subscription->try_pop() )
            {
                const auto* mb = std::get_if<grab::MouseButton>( &btn_event->payload );
                if( mb == nullptr )
                {
                    continue;
                }
                if( mb->button == wheel_down &&
                    btn_event->kind == grab::EventKind::MouseButtonDown )
                {
                    ++wheel_downs;
                }
                if( mb->button != primary_button )
                {
                    continue;
                }
                if( btn_event->kind == grab::EventKind::MouseButtonDown &&
                    mb->position.has_value() )
                {
                    down_pos = mb->position;
                }
                else if( btn_event->kind == grab::EventKind::MouseButtonUp &&
                         mb->position.has_value() )
                {
                    up_pos = mb->position;
                }
            }
            if( wheel_downs > 0 )
            {
                wheel_seen = "scrolled";
            }
            if( down_pos.has_value() && up_pos.has_value() &&
                live->rect_.contains( down_pos->x, down_pos->y ) &&
                live->rect_.contains( up_pos->x, up_pos->y ) )
            {
                click_seen = "clicked";
            }
            std::cout << "  events    " << wheel_downs << " wheel notch(es), click "
                      << click_seen << "  hold "
                      << static_cast<int>( hold_s * 1000.0 ) << " ms\n";
        }
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::ButtonClick,
                                            .subject_ = wheel_subject,
                                            .value_   = wheel_seen } );
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::ButtonClick,
                                            .subject_ = subject,
                                            .value_   = click_seen } );

        std::this_thread::sleep_for( std::chrono::milliseconds{ react_ms } );

        // ── 6. OBSERVE ──────────────────────────────────────────────────────
        std::string a11y_after = "(unresolved)";
        for( int attempt = 0; attempt < poll_tries; ++attempt )
        {
            if( auto again = resolve_named( **session,
                                            { clicked_label, idle_label } );
                again.has_value() )
            {
                a11y_after = again->name_;
                if( a11y_after == clicked_label )
                {
                    break;
                }
            }
            std::this_thread::sleep_for( std::chrono::milliseconds{ poll_ms } );
        }
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::A11yName,
                                            .subject_ = subject,
                                            .value_   = a11y_after } );
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
                          << static_cast<int>( moved ) << " (>= "
                          << static_cast<int>( colour_match )
                          << " counts as changed)\n";
            }
        }
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::PixelColour,
                                            .subject_ = subject,
                                            .value_   = flipped } );
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
