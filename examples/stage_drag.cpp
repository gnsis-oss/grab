// ┌──────────────────────────────────────────────────────────────────────────┐
// │  stage_drag — rung 8 of the capability ladder: press · drag · release.   │
// │                                                                          │
// │  ONE page. An app icon on the left, a drop zone on the right — the       │
// │  macOS-install gesture: grab the icon, carry it across the page with     │
// │  the button held, drop it into the zone. The page moves the icon with    │
// │  the cursor while the button is down and, when the icon's centre is      │
// │  released inside the zone, snaps it in, flips both elements' colours     │
// │  and renames them APP → INSTALLED, EMPTY → OCCUPIED.                     │
// │                                                                          │
// │  The drop is then checked on channels that cannot cover for each other:  │
// │                                                                          │
// │    a11y    both accessible names flipped, and the icon's LIVE bounds     │
// │            ended inside the zone's live bounds                           │
// │    pixel   the zone's mean colour changed from "empty" to "occupied"     │
// │    device  the press landed inside the icon and the release inside the   │
// │            zone, both read back from the X server's own event stream     │
// │                                                                          │
// │  The press is paired with a release on EVERY exit path — a button left   │
// │  down survives this process and is still down for the next application. │
// │                                                                          │
// │    stage_drag                 headless, on a display it creates          │
// │    stage_drag --session       the display you are already on            │
// │    stage_drag --trail         approach and carry drawn on the overlay    │
// │    stage_drag --trail --watch same, in a Xephyr WINDOW you can watch     │
// │    stage_drag --keep          leave the session up afterwards            │
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
    //
    // Absolutely positioned, explicit width and height, box-sizing:border-box,
    // so every number below is exact regardless of installed fonts.

    constexpr int           icon_x = 120;
    constexpr int           icon_y = 260;
    constexpr int           icon_w = 140;
    constexpr int           icon_h = 140;

    constexpr int           zone_x = 620;
    constexpr int           zone_y = 200;
    constexpr int           zone_w = 260;
    constexpr int           zone_h = 260;

    constexpr const char*   icon_idle_label = "APP";
    constexpr const char*   icon_done_label = "INSTALLED";
    constexpr const char*   zone_idle_label = "EMPTY";
    constexpr const char*   zone_done_label = "OCCUPIED";

    // Far apart in RGB and clear of the overlay palette, so "it changed" is
    // never a judgement call about a shade.
    constexpr const char*   icon_idle_fill = "#1d4e89";
    constexpr const char*   icon_done_fill = "#2a9d3a";
    constexpr const char*   zone_idle_fill = "#e8e8e8";
    constexpr const char*   zone_done_fill = "#bff0c4";

    constexpr const char*   title_marker = "Stage Drag";

    constexpr std::uint32_t primary_button = 1U;    // X11 primary pointer button

    constexpr int           viewport_w = 1'000;
    constexpr int           viewport_h = 700;

    constexpr const char*   icon_subject    = "app_icon";
    constexpr const char*   zone_subject    = "drop_zone";
    constexpr const char*   drag_subject    = "drag";
    constexpr const char*   landing_subject = "landing";

    constexpr grab::overlay::Color cyan{ .r = 0U, .g = 217U, .b = 255U, .a = 242U };
    constexpr grab::overlay::Color green{ .r = 51U, .g = 255U, .b = 89U, .a = 242U };
    constexpr grab::overlay::Color amber{ .r = 255U, .g = 184U, .b = 26U, .a = 242U };

    constexpr double               stroke_px       = 3.0;
    constexpr double               trail_stroke_px = 2.0;
    // Park bottom-left of the WINDOW, clear of both rects, so the approach
    // crosses the page — as fractions of the window's live frame, because on
    // a real desktop the window manager decides where the window is and an
    // absolute park can sit over another application entirely.
    constexpr double               park_fx     = 0.08;
    constexpr double               park_fy     = 0.88;
    // The trail yields to the motion, never the reverse — see stage_button.
    constexpr auto                 trail_slack = std::chrono::milliseconds{ 8 };

    // The carry aims at a subrect around the zone's centre rather than the
    // whole zone: the icon rides the cursor at the offset it was grabbed by,
    // so its CENTRE can sit up to half an icon away from the cursor. A cursor
    // released inside this subrect keeps the icon's centre inside the zone
    // for every grab point the icon admits.
    constexpr double        drop_margin = 90.0;

    constexpr std::uint64_t seed        = 0X5'D1'DE'00'08ULL;
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
        // Both elements report state in TWO channels at once — accessible name
        // and fill colour flip together — so the observers below read one
        // event through independent instruments. The dragging itself is plain
        // mousedown / mousemove / mouseup: the icon rides the cursor at the
        // offset it was grabbed by, exactly the macOS install gesture.
        const auto px = []( int value )
        {
            return std::to_string( value ) + "px";
        };
        return std::string{ "<!doctype html>\n"
                            "<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
                            "<title>Stage Drag — grab</title>\n"
                            "<style>\n"
                            "  html,body{margin:0;padding:0;background:#f4f4f4;}\n"
                            "  #app_icon{position:absolute;box-sizing:border-box;" } +
               "left:" + px( icon_x ) + ";top:" + px( icon_y ) +
               ";width:" + px( icon_w ) + ";height:" + px( icon_h ) +
               ";background:" + icon_idle_fill +
               ";color:#fff;border:0;font:600 24px sans-serif;cursor:grab;}\n"
               "  #drop_zone{position:absolute;box-sizing:border-box;"
               "left:" + px( zone_x ) + ";top:" + px( zone_y ) +
               ";width:" + px( zone_w ) + ";height:" + px( zone_h ) +
               ";background:" + zone_idle_fill +
               ";border:6px dashed #8a8a8a;color:#555;font:600 22px sans-serif;}\n"
               "</style></head><body>\n"
               "<button id=\"drop_zone\" aria-label=\"" + zone_idle_label + "\">" +
               zone_idle_label + "</button>\n"
               "<button id=\"app_icon\" aria-label=\"" + icon_idle_label + "\">" +
               icon_idle_label + "</button>\n"
               "<script>\n"
               "  var icon=document.getElementById('app_icon');\n"
               "  var zone=document.getElementById('drop_zone');\n"
               "  var drag=null;\n"
               "  icon.addEventListener('mousedown',function(e){\n"
               "    drag={dx:e.clientX-icon.offsetLeft,"
               "dy:e.clientY-icon.offsetTop};\n"
               "    e.preventDefault();\n"
               "  });\n"
               "  document.addEventListener('mousemove',function(e){\n"
               "    if(!drag) return;\n"
               "    icon.style.left=(e.clientX-drag.dx)+'px';\n"
               "    icon.style.top=(e.clientY-drag.dy)+'px';\n"
               "  });\n"
               "  document.addEventListener('mouseup',function(e){\n"
               "    if(!drag) return;\n"
               "    drag=null;\n"
               "    var ir=icon.getBoundingClientRect();\n"
               "    var zr=zone.getBoundingClientRect();\n"
               "    var cx=ir.left+ir.width/2, cy=ir.top+ir.height/2;\n"
               "    if(cx>=zr.left&&cx<=zr.right&&cy>=zr.top&&cy<=zr.bottom){\n"
               "      icon.style.left=(zr.left+(zr.width-ir.width)/2)+'px';\n"
               "      icon.style.top=(zr.top+(zr.height-ir.height)/2)+'px';\n"
               "      icon.style.background='" + std::string{ icon_done_fill } + "';\n"
               "      icon.setAttribute('aria-label','" + icon_done_label + "');\n"
               "      icon.textContent='" + icon_done_label + "';\n"
               "      zone.style.background='" + zone_done_fill + "';\n"
               "      zone.style.borderColor='" + icon_done_fill + "';\n"
               "      zone.setAttribute('aria-label','" + zone_done_label + "');\n"
               "      zone.textContent='" + zone_done_label + "';\n"
               "    } else {\n"
               "      icon.style.left='" + px( icon_x ) + "';\n"
               "      icon.style.top='" + px( icon_y ) + "';\n"
               "    }\n"
               "  });\n"
               "</script></body></html>\n";
    }

    [[nodiscard]]
    stage::Scene
    build_scene()
    {
        stage::Scene scene;
        scene.id_ = "drag";
        scene.pages_.push_back( stage::ScenePage{ .name_   = "drag",
                                                  .html_   = page_html(),
                                                  .marker_ = title_marker } );
        scene.viewport_ = stage::ViewportSpec{ .viewport_w_ = viewport_w,
                                               .viewport_h_ = viewport_h,
                                               .document_h_ = viewport_h };
        scene.frames_   = { "01-baseline", "02-carry", "03-after" };

        // DECLARED BEFORE THE ACT.
        const auto expect = [&]( std::string    name,
                                 stage::Observe observe,
                                 std::string    subject,
                                 std::string    value )
        {
            scene.expect_.push_back( stage::Expectation{ .name_    = std::move( name ),
                                                         .observe_ = observe,
                                                         .subject_ = std::move( subject ),
                                                         .value_   = std::move( value ),
                                                         .tolerance_ = 0.0,
                                                         .low_       = 0.0,
                                                         .high_      = 0.0,
                                                         .ranged_    = false } );
        };
        expect( "overlay_is_live", stage::Observe::Capability, "overlay", "live" );
        expect( "icon_size_as_authored",
                stage::Observe::A11yBounds,
                icon_subject,
                std::to_string( icon_w ) + "x" + std::to_string( icon_h ) );
        expect( "press_inside_icon",
                stage::Observe::CursorPosition,
                icon_subject,
                "inside" );
        expect( "drag_pair_observed",
                stage::Observe::ButtonClick,
                drag_subject,
                "dragged" );
        expect( "icon_reports_installed",
                stage::Observe::A11yName,
                icon_subject,
                icon_done_label );
        expect( "zone_reports_occupied",
                stage::Observe::A11yName,
                zone_subject,
                zone_done_label );
        expect( "icon_landed_in_zone",
                stage::Observe::A11yBounds,
                landing_subject,
                "inside" );
        expect( "zone_pixels_flipped",
                stage::Observe::PixelColour,
                zone_subject,
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

    // An element's accessible name and screen rect, read live.
    struct Live
    {
            std::string             name_;
            view::ViewRect          rect_{};
            grab::CoordinateSpaceId space_{};
    };

    // Resolve the document button whose accessible name is one of `names`.
    // Scoped to the DOCUMENT so the window manager's own titlebar buttons can
    // never match — the lesson stage_button learnt by clicking "Minimize".
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

    // The press is owned by whoever made it. A primary button left down
    // survives this process — it is still down for the next application on
    // the display — so every exit path out of the carry, including the error
    // and exception ones, must release. This is that guarantee.
    class HeldButton
    {
        public:

            explicit HeldButton( grab::Input& input ) :
                input_( input )
            {
            }

            HeldButton( const HeldButton& )            = delete;
            HeldButton& operator=( const HeldButton& ) = delete;
            HeldButton( HeldButton&& )                 = delete;
            HeldButton& operator=( HeldButton&& )      = delete;

            void
            press()
            {
                if( auto pressed = input_.press(); pressed.has_value() )
                {
                    held_ = true;
                }
            }

            void
            release()
            {
                if( held_ )
                {
                    held_ = false;
                    ( void )input_.release();
                }
            }

            [[nodiscard]]
            bool
            held() const noexcept
            {
                return held_;
            }

            ~HeldButton()
            {
                release();
            }

        private:

            grab::Input& input_;
            bool         held_ = false;
    };

    struct Options
    {
            std::string           display = ":71";
            std::filesystem::path out     = "stage-drag";
            std::string           host_display;
            bool                  attach = false;
            bool                  keep   = false;
            bool                  trail  = false;
    };

    void
    usage()
    {
        std::cout << "stage_drag — rung 8: grab an icon, carry it, drop it\n\n"
                     "  --display :N   display to create (ignored with --session)\n"
                     "  --out DIR      where the page, frames and scorecard land\n"
                     "  --session      drive the display you are already on\n"
                     "  --watch        show the nested display in a window here\n"
                     "  --trail        draw the approach and the carry\n"
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
                std::cerr << "stage_drag: " << flag << ' ' << reason << "\n\n";
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
                    return bad( "requires a display, e.g. :71" );
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
                // "Profile Missing".
                options.out = std::filesystem::absolute( raw );
            }
            else if( flag == "--session" )
            {
                options.attach = true;
                // NOLINTNEXTLINE(concurrency-mt-unsafe)
                const char* const ambient = std::getenv( "DISPLAY" );
                if( ambient == nullptr || ambient[0] == '\0' )
                {
                    std::cerr << "stage_drag: --session needs DISPLAY set\n";
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
            std::cerr << "stage_drag: --watch and --session are mutually exclusive\n\n";
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
    // GRAB_LOG_TAGS / GRAB_LOG_FILE) turns library logging on.
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
    const std::filesystem::path page = options.out / "drag.html";
    {
        std::ofstream out( page );
        out << scene.pages_.front().html_;
    }
    std::cout << "AUTHOR\n  icon      (" << icon_x << "," << icon_y << " " << icon_w
              << "x" << icon_h << ")\n  zone      (" << zone_x << "," << zone_y << " "
              << zone_w << "x" << zone_h << ") -> " << page << '\n';

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
    // Point AT-SPI at the host's own session bus — see stage_button.
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

        // The X server's own record of the presses this run makes.
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
        std::optional<Live> icon;
        std::optional<Live> zone;
        for( int attempt = 0; attempt < poll_tries; ++attempt )
        {
            icon = resolve_named( **session, { icon_idle_label } );
            zone = resolve_named( **session, { zone_idle_label } );
            if( icon.has_value() && zone.has_value() )
            {
                break;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds{ poll_ms } );
        }
        if( !icon.has_value() || !zone.has_value() )
        {
            std::cerr << "the " << ( icon.has_value() ? "drop zone" : "app icon" )
                      << " never appeared in the accessibility tree\n";
            host.stop();
            return 1;
        }
        std::cout << "  icon      a11y (" << static_cast<int>( icon->rect_.x_ ) << ","
                  << static_cast<int>( icon->rect_.y_ ) << " "
                  << static_cast<int>( icon->rect_.w_ ) << "x"
                  << static_cast<int>( icon->rect_.h_ ) << ") name=\"" << icon->name_
                  << "\"\n";
        std::cout << "  zone      a11y (" << static_cast<int>( zone->rect_.x_ ) << ","
                  << static_cast<int>( zone->rect_.y_ ) << " "
                  << static_cast<int>( zone->rect_.w_ ) << "x"
                  << static_cast<int>( zone->rect_.h_ ) << ") name=\"" << zone->name_
                  << "\"\n";
        seen.push_back( stage::Observation{
            .observe_ = stage::Observe::A11yBounds,
            .subject_ = icon_subject,
            .value_   = std::to_string( static_cast<int>( icon->rect_.w_ ) ) + "x" +
                      std::to_string( static_cast<int>( icon->rect_.h_ ) ) } );

        // ── 4. DRAW ─────────────────────────────────────────────────────────
        const auto goal = [&]( const view::ViewRect&       rect,
                               const grab::overlay::Color& colour )
        {
            if( overlay == nullptr )
            {
                return;
            }
            ( void )overlay->add( grab::overlay::Shape{
                .geometry = grab::overlay::Rect{ .bounds =
                                                     grab::SpaceRect{ .x = rect.x_,
                                                                      .y = rect.y_,
                                                                      .w = rect.w_,
                                                                      .h = rect.h_,
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
        goal( icon->rect_, cyan );
        goal( zone->rect_, green );
        std::this_thread::sleep_for( std::chrono::milliseconds{ announce_ms } );

        // Baseline BEFORE anything moves: the zone must read "empty" here.
        auto before = ( *screen ).display();
        if( before.has_value() )
        {
            pixel::write_ppm( *before, options.out / "01-baseline.ppm" );
        }

        // ── 5. APPROACH · GRAB · CARRY · DROP ───────────────────────────────
        motion::Rng rng{ seed };
        // Park relative to the window's live frame, wherever the window
        // manager put it. Falls back to the icon's own rect if the window
        // list momentarily fails — a short approach beats no park.
        auto park_x = static_cast<std::int16_t>( icon->rect_.x_ );
        auto park_y = static_cast<std::int16_t>( icon->rect_.y_ );
        if( const auto summary = host.browser_window(); summary.has_value() )
        {
            park_x = static_cast<std::int16_t>(
                static_cast<double>( summary->bounds.x ) +
                ( static_cast<double>( summary->bounds.width ) * park_fx )
            );
            park_y = static_cast<std::int16_t>(
                static_cast<double>( summary->bounds.y ) +
                ( static_cast<double>( summary->bounds.height ) * park_fy )
            );
        }
        ( void )( *input ).move( park_x, park_y );
        std::this_thread::sleep_for( std::chrono::milliseconds{ settle_ms } );

        // The shared trail: both legs append here, and every stroke is
        // retired after the release so the observers read the page.
        std::vector<motion::Vec2>           pending;
        std::vector<grab::overlay::ShapeId> trail_ids;
        const auto                          stroke =
            [&]( const grab::overlay::Color& colour )
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
                .stroke   = grab::overlay::StrokeStyle{ .color    = colour,
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

        // Walk one planned movement, drawing as it goes. Returns where the
        // cursor actually ended.
        const auto walk = [&]( const motion::Movement&     movement,
                               const grab::overlay::Color& colour ) -> motion::Vec2
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
            motion::Vec2 last{};
            for( std::size_t step = 0U; step < movement.samples_.size(); ++step )
            {
                const motion::Sample& sample = movement.samples_[step];
                std::this_thread::sleep_until( deadline_of( sample ) );
                ( void )( *input ).move( static_cast<std::int16_t>( sample.x_ ),
                                         static_cast<std::int16_t>( sample.y_ ) );
                last = motion::Vec2{ .x_ = static_cast<double>( sample.x_ ),
                                     .y_ = static_cast<double>( sample.y_ ) };
                if( !options.trail )
                {
                    continue;
                }
                pending.push_back( last );
                const bool final_sample = step + 1U == movement.samples_.size();
                if( final_sample ||
                    ( deadline_of( movement.samples_[step + 1U] ) -
                      std::chrono::steady_clock::now() ) > trail_slack )
                {
                    stroke( colour );
                }
            }
            return last;
        };

        // Leg 1 — approach the icon.
        const auto   start = ( *input ).position();
        motion::Vec2 cursor{ start.has_value() ? static_cast<double>( start->x )
                                               : static_cast<double>( park_x ),
                             start.has_value() ? static_cast<double>( start->y )
                                               : static_cast<double>( park_y ) };
        const motion::Rect icon_aim{ icon->rect_.x_,
                                     icon->rect_.y_,
                                     icon->rect_.w_,
                                     icon->rect_.h_ };
        cursor = walk( motion::plan_move( rng, cursor, icon_aim, motion::MotionConfig{} ),
                       amber );

        // Where the press ACTUALLY happens, read from the X server.
        const auto at_press     = ( *input ).position();
        const bool press_inside = at_press.has_value() &&
                                  icon->rect_.contains(
                                      static_cast<double>( at_press->x ),
                                      static_cast<double>( at_press->y ) );
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::CursorPosition,
                                            .subject_ = icon_subject,
                                            .value_ = press_inside ? "inside"
                                                                   : "outside" } );
        if( at_press.has_value() )
        {
            std::cout << "  press     (" << at_press->x << "," << at_press->y << ") "
                      << ( press_inside ? "inside" : "OUTSIDE" ) << " the icon\n";
        }

        // Grab it. From here to the release, every exit path lets go.
        HeldButton held{ *input };
        held.press();
        // A human settles a grip before pulling: sampled, like the click hold.
        const double grip_s = std::exp( rng.normal( std::log( 0.120 ), 0.30 ) );
        std::this_thread::sleep_for( std::chrono::duration<double>( grip_s ) );

        // Leg 2 — carry to the zone's centre subrect (see drop_margin).
        const motion::Rect drop_aim{ zone->rect_.x_ + drop_margin,
                                     zone->rect_.y_ + drop_margin,
                                     zone->rect_.w_ - ( 2.0 * drop_margin ),
                                     zone->rect_.h_ - ( 2.0 * drop_margin ) };
        cursor = walk( motion::plan_move( rng, cursor, drop_aim, motion::MotionConfig{} ),
                       cyan );
        std::cout << "  carry     ended (" << static_cast<int>( cursor.x_ ) << ","
                  << static_cast<int>( cursor.y_ ) << ") with the button held\n";

        if( options.trail && overlay != nullptr )
        {
            // The one frame the whole example exists for: the icon mid-carry,
            // the button held, both trails on screen over the page.
            ( void )overlay->flush();
            const auto during = ( *screen ).display();
            if( during.has_value() )
            {
                pixel::write_ppm( *during, options.out / "02-carry.ppm" );
            }
        }

        const auto at_release = ( *input ).position();
        std::this_thread::sleep_for( std::chrono::milliseconds{ 80 } );
        held.release();

        // Trails have done their job; retire them before any observer reads.
        if( options.trail && overlay != nullptr && !trail_ids.empty() )
        {
            for( const grab::overlay::ShapeId id : trail_ids )
            {
                ( void )overlay->remove( id );
            }
            ( void )overlay->flush();
        }

        std::this_thread::sleep_for( std::chrono::milliseconds{ 50 } );

        // The X server's own record: a DOWN inside the icon's rect-at-press
        // and an UP inside the zone. What this program believes proves nothing.
        std::string drag_observed = "not observed";
        if( button_subscription.has_value() )
        {
            std::optional<grab::SpacePoint> down_pos;
            std::optional<grab::SpacePoint> up_pos;
            while( auto btn_event = button_subscription->try_pop() )
            {
                const auto* mb = std::get_if<grab::MouseButton>( &btn_event->payload );
                if( mb == nullptr || mb->button != primary_button )
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
            if( down_pos.has_value() && up_pos.has_value() &&
                icon->rect_.contains( down_pos->x, down_pos->y ) &&
                zone->rect_.contains( up_pos->x, up_pos->y ) )
            {
                drag_observed = "dragged";
            }
        }
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::ButtonClick,
                                            .subject_ = drag_subject,
                                            .value_   = drag_observed } );
        if( at_release.has_value() )
        {
            std::cout << "  release   (" << at_release->x << "," << at_release->y
                      << ")  grip " << static_cast<int>( grip_s * 1000.0 ) << " ms\n";
        }

        std::this_thread::sleep_for( std::chrono::milliseconds{ react_ms } );

        // ── 6. OBSERVE ──────────────────────────────────────────────────────
        std::optional<Live> icon_after;
        for( int attempt = 0; attempt < poll_tries; ++attempt )
        {
            icon_after = resolve_named( **session,
                                        { icon_done_label, icon_idle_label } );
            if( icon_after.has_value() && icon_after->name_ == icon_done_label )
            {
                break;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds{ poll_ms } );
        }
        const auto zone_after = resolve_named( **session,
                                               { zone_done_label, zone_idle_label } );
        seen.push_back( stage::Observation{
            .observe_ = stage::Observe::A11yName,
            .subject_ = icon_subject,
            .value_ = icon_after.has_value() ? icon_after->name_ : "(unresolved)" } );
        seen.push_back( stage::Observation{
            .observe_ = stage::Observe::A11yName,
            .subject_ = zone_subject,
            .value_ = zone_after.has_value() ? zone_after->name_ : "(unresolved)" } );
        std::cout << "  a11y      icon=\""
                  << ( icon_after.has_value() ? icon_after->name_ : "(unresolved)" )
                  << "\" zone=\""
                  << ( zone_after.has_value() ? zone_after->name_ : "(unresolved)" )
                  << "\"\n";

        // The landing, from the LIVE a11y bounds: the icon's centre must sit
        // inside the zone's rect, both read after the drop.
        std::string landed = "(unresolved)";
        if( icon_after.has_value() && zone_after.has_value() )
        {
            landed = zone_after->rect_.contains( icon_after->rect_.center_x(),
                                                 icon_after->rect_.center_y() )
                       ? "inside"
                       : "outside";
            std::cout << "  landing   icon centre ("
                      << static_cast<int>( icon_after->rect_.center_x() ) << ","
                      << static_cast<int>( icon_after->rect_.center_y() ) << ") "
                      << landed << " the zone\n";
        }
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::A11yBounds,
                                            .subject_ = landing_subject,
                                            .value_   = landed } );

        std::this_thread::sleep_for( std::chrono::milliseconds{ settle_ms } );
        auto        after   = ( *screen ).display();
        std::string flipped = "unreadable";
        if( before.has_value() && after.has_value() )
        {
            const auto was = pixel::mean_colour( *before, zone->rect_ );
            const auto now = pixel::mean_colour( *after, zone->rect_ );
            if( was.has_value() && now.has_value() )
            {
                const double moved = pixel::distance( *was, *now );
                flipped            = moved >= colour_match ? "changed" : "unchanged";
                std::cout << "  pixel     zone mean colour moved "
                          << static_cast<int>( moved ) << " (>= "
                          << static_cast<int>( colour_match )
                          << " counts as changed)\n";
            }
        }
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::PixelColour,
                                            .subject_ = zone_subject,
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
