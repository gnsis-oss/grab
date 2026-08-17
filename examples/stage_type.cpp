// ┌──────────────────────────────────────────────────────────────────────────┐
// │  stage_type — rung 6 of the capability ladder: the keyboard.             │
// │                                                                          │
// │  ONE page: a labelled MESSAGE field and a SEND button. The cursor        │
// │  travels to the field and clicks it to take focus, then a whole          │
// │  paragraph is typed LETTER BY LETTER at a human rhythm — jittered        │
// │  per-key gaps, a breath after each word, longer at commas, longer        │
// │  still at full stops, and the occasional mid-sentence think. Then the    │
// │  cursor travels to SEND and presses it; the page stamps the button       │
// │  SENT and writes what it received into a receipt element.                │
// │                                                                          │
// │  Checked across channels that cannot cover for each other:              │
// │                                                                          │
// │    a11y    the field is the LABELLED entry (name MESSAGE); its value     │
// │            equals the paragraph EXACTLY before send; the button flips    │
// │            SEND -> SENT; the receipt's name carries the very text the    │
// │            page's own script received                                    │
// │    device  the X server's event stream carries at least one key-down     │
// │            per typed character, and both click pairs land inside their   │
// │            targets                                                       │
// │    pixel   the send button's fill flips on the click                     │
// │                                                                          │
// │    stage_type                 headless, on a display it creates          │
// │    stage_type --session       the display you are already on            │
// │    stage_type --trail         both approaches drawn on the overlay       │
// │    stage_type --trail --watch same, in a Xephyr WINDOW you can watch     │
// │    stage_type --keep          leave the session up afterwards            │
// └──────────────────────────────────────────────────────────────────────────┘

#include "support/host.hpp"
#include "support/motion/noise.hpp"
#include "support/overlay_align.hpp"
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

    constexpr int         viewport_w = 1'000;
    constexpr int         viewport_h = 700;

    constexpr int         field_x = 150;
    constexpr int         field_y = 120;
    constexpr int         field_w = 700;
    constexpr int         field_h = 240;

    constexpr int         send_x = 150;
    constexpr int         send_y = 410;
    constexpr int         send_w = 200;
    constexpr int         send_h = 80;

    constexpr const char* field_label    = "MESSAGE";
    constexpr const char* send_idle      = "SEND";
    constexpr const char* send_done      = "SENT";
    constexpr const char* receipt_empty  = "RECEIPT EMPTY";
    constexpr const char* receipt_prefix = "RECEIPT ";
    constexpr const char* send_idle_fill = "#1d4e89";
    constexpr const char* send_done_fill = "#2a9d3a";
    constexpr const char* title_marker   = "Stage Type";

    // The paragraph, typed one character at a time. ASCII only, so every
    // character is reachable through the default layout.
    constexpr const char* paragraph =
        "The quick brown fox jumps over the lazy dog. It pauses at commas, "
        "breathes at full stops, and then, letter by letter, it finishes "
        "the note.";

    constexpr std::uint32_t primary_button = 1U;    // X11 primary pointer button

    constexpr const char*   field_subject   = "msg";
    constexpr const char*   send_subject    = "send";
    constexpr const char*   receipt_subject = "receipt";
    constexpr const char*   keys_subject    = "keys";

    constexpr grab::overlay::Color cyan{ .r = 0U, .g = 217U, .b = 255U, .a = 242U };
    constexpr grab::overlay::Color amber{ .r = 255U, .g = 184U, .b = 26U, .a = 242U };

    constexpr double               stroke_px       = 3.0;
    constexpr double               trail_stroke_px = 2.0;
    constexpr auto                 trail_slack = std::chrono::milliseconds{ 8 };

    // Park bottom-left of the window (fractions of its live frame).
    constexpr double        park_fx = 0.08;
    constexpr double        park_fy = 0.88;

    // ── The typing rhythm ───────────────────────────────────────────────────
    //
    // Log-normal per-key gaps, plus structure: a breath after each word, a
    // longer beat at a comma, longer still at a full stop, and now and then
    // a mid-sentence think. All sampled from the run's seeded Rng, so two
    // runs type with the same rhythm.
    constexpr double        key_gap_ln_mean  = 4.01;    // ln(55 ms)
    constexpr double        key_gap_ln_sigma = 0.35;
    constexpr double        word_gap_ms      = 90.0;
    constexpr double        comma_gap_ms     = 260.0;
    constexpr double        stop_gap_ms      = 450.0;
    constexpr double        think_gap_ms     = 650.0;
    constexpr double        think_chance     = 0.03;

    constexpr std::uint64_t seed        = 0X5'D1'DE'00'06ULL;
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
        return std::string{ "<!doctype html>\n"
                            "<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
                            "<title>Stage Type — grab</title>\n"
                            "<style>\n"
                            "  html,body{margin:0;padding:0;background:#f4f4f4;}\n"
                            "  #msg{position:absolute;box-sizing:border-box;" } +
               "left:" + px( field_x ) + ";top:" + px( field_y ) +
               ";width:" + px( field_w ) + ";height:" + px( field_h ) +
               ";font:400 22px sans-serif;padding:12px;resize:none;"
               "border:3px solid #8fa3c0;background:#ffffff;}\n"
               "  #send{position:absolute;box-sizing:border-box;"
               "left:" + px( send_x ) + ";top:" + px( send_y ) +
               ";width:" + px( send_w ) + ";height:" + px( send_h ) +
               ";background:" + send_idle_fill +
               ";color:#fff;border:0;font:600 32px sans-serif;}\n"
               "  #receipt{position:absolute;left:" + px( send_x + send_w + 40 ) +
               ";top:" + px( send_y ) + ";width:" +
               px( viewport_w - ( send_x + send_w + 80 ) ) +
               ";font:400 16px sans-serif;color:#33415c;}\n"
               "</style></head><body>\n"
               "<textarea id=\"msg\" aria-label=\"" + field_label +
               "\"></textarea>\n"
               "<button id=\"send\" aria-label=\"" + send_idle + "\">" + send_idle +
               "</button>\n"
               "<button id=\"receipt\" aria-label=\"" + receipt_empty +
               "\" style=\"position:absolute;left:" + px( send_x + send_w + 40 ) +
               ";top:" + px( send_y ) + ";width:" +
               px( viewport_w - ( send_x + send_w + 80 ) ) + ";height:" +
               px( send_h ) +
               ";background:#e8e8e8;border:2px dashed #8a8a8a;color:#555;"
               "font:400 14px sans-serif;\">" + receipt_empty +
               "</button>\n"
               "<script>\n"
               "  var m=document.getElementById('msg');\n"
               "  var s=document.getElementById('send');\n"
               "  var r=document.getElementById('receipt');\n"
               "  s.addEventListener('click',function(){\n"
               "    s.style.background='" + send_done_fill + "';\n"
               "    s.setAttribute('aria-label','" + send_done + "');\n"
               "    s.textContent='" + send_done + "';\n"
               "    var got='" + receipt_prefix + "'+m.value;\n"
               "    r.setAttribute('aria-label',got);\n"
               "    r.textContent=got;\n"
               "  });\n"
               "</script></body></html>\n";
    }

    [[nodiscard]]
    stage::Scene
    build_scene()
    {
        stage::Scene scene;
        scene.id_ = "type";
        scene.pages_.push_back( stage::ScenePage{ .name_   = "type",
                                                  .html_   = page_html(),
                                                  .marker_ = title_marker } );
        scene.viewport_ = stage::ViewportSpec{ .viewport_w_ = viewport_w,
                                               .viewport_h_ = viewport_h,
                                               .document_h_ = viewport_h };
        scene.frames_   = { "01-empty", "02-typed", "03-after" };

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
        expect( "field_is_labelled",
                stage::Observe::A11yName,
                field_subject,
                field_label );
        expect( "click_inside_field",
                stage::Observe::CursorPosition,
                field_subject,
                "inside" );
        expect( "keystrokes_observed",
                stage::Observe::ButtonClick,
                keys_subject,
                "typed" );
        expect( "typed_value_matches",
                stage::Observe::A11yValue,
                field_subject,
                paragraph );
        expect( "click_inside_send",
                stage::Observe::CursorPosition,
                send_subject,
                "inside" );
        expect( "send_click_observed",
                stage::Observe::ButtonClick,
                send_subject,
                "clicked" );
        expect( "send_reports_sent",
                stage::Observe::A11yName,
                send_subject,
                send_done );
        expect( "receipt_matches",
                stage::Observe::A11yName,
                receipt_subject,
                std::string{ receipt_prefix } + paragraph );
        expect( "pixels_flipped", stage::Observe::PixelColour, send_subject,
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
            std::string             text_;
            view::ViewRect          rect_{};
            grab::CoordinateSpaceId space_{};
    };

    // Resolve a document node of `role_id` whose accessible name is one of
    // `names`. The document scope keeps window-manager furniture out.
    [[nodiscard]]
    std::optional<Live>
    resolve_role_named( grab::Session&                          session,
                        grab::RoleId                            role_id,
                        std::initializer_list<std::string_view> names )
    {
        if( auto synced = session.resync(); !synced.has_value() )
        {
            return std::nullopt;
        }
        auto matches =
            session.resolve_all( grab::sel::role( role_id ).and_(
                grab::sel::descendant_of( grab::sel::role( grab::role::document ) )
            ) );
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
                        .text_  = info.text,
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

    struct Options
    {
            std::string           display = ":68";
            std::filesystem::path out     = "stage-type";
            std::string           host_display;
            bool                  attach = false;
            bool                  keep   = false;
            bool                  trail  = false;
    };

    void
    usage()
    {
        std::cout << "stage_type — rung 6: focus a field, type a paragraph, send\n\n"
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
                std::cerr << "stage_type: " << flag << ' ' << reason << "\n\n";
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
                    return bad( "requires a display, e.g. :68" );
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
                    std::cerr << "stage_type: --session needs DISPLAY set\n";
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
            std::cerr << "stage_type: --watch and --session are mutually exclusive\n\n";
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
    const std::filesystem::path page = options.out / "type.html";
    {
        std::ofstream out( page );
        out << scene.pages_.front().html_;
    }
    std::cout << "AUTHOR\n  field     (" << field_x << "," << field_y << " " << field_w
              << "x" << field_h << ")\n  send      (" << send_x << "," << send_y << " "
              << send_w << "x" << send_h << ")\n  text      "
              << std::string_view{ paragraph }.size() << " characters -> " << page
              << '\n';

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

        // The X server's own record: every keystroke and both clicks.
        std::optional<grab::Subscription> event_subscription;
        {
            grab::SubscriptionScope scope;
            scope.kinds = { grab::EventKind::KeyDown,
                            grab::EventKind::MouseButtonDown,
                            grab::EventKind::MouseButtonUp };
            auto sub    = ( *session )->watch( scope );
            if( sub.has_value() )
            {
                event_subscription = std::move( *sub );
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
        std::optional<Live> field;
        std::optional<Live> send;
        for( int attempt = 0; attempt < poll_tries; ++attempt )
        {
            field = resolve_role_named( **session, grab::role::entry,
                                        { field_label } );
            send  = resolve_role_named( **session, grab::role::button,
                                        { send_idle } );
            if( field.has_value() && send.has_value() )
            {
                break;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds{ poll_ms } );
        }
        if( !field.has_value() || !send.has_value() )
        {
            std::cerr << "the " << ( field.has_value() ? "send button" : "entry" )
                      << " never appeared in the accessibility tree\n";
            host.stop();
            return 1;
        }
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::A11yName,
                                            .subject_ = field_subject,
                                            .value_   = field->name_ } );
        std::cout << "  field     a11y (" << static_cast<int>( field->rect_.x_ ) << ","
                  << static_cast<int>( field->rect_.y_ ) << " "
                  << static_cast<int>( field->rect_.w_ ) << "x"
                  << static_cast<int>( field->rect_.h_ ) << ") name=\"" << field->name_
                  << "\"\n  send      a11y (" << static_cast<int>( send->rect_.x_ )
                  << "," << static_cast<int>( send->rect_.y_ ) << " "
                  << static_cast<int>( send->rect_.w_ ) << "x"
                  << static_cast<int>( send->rect_.h_ ) << ") name=\"" << send->name_
                  << "\"\n";

        auto empty_frame = ( *screen ).display();
        if( empty_frame.has_value() )
        {
            pixel::write_ppm( *empty_frame, options.out / "01-empty.ppm" );
        }

        // Where do overlay shapes ACTUALLY land? Measured, at the field.
        const auto omap = ladder::view::align::measure(
            overlay, *screen, space, field->rect_.x_, field->rect_.y_ );

        // ── 4. THE ACT ──────────────────────────────────────────────────────
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

        // Approach a target on a human trajectory and click it. Returns
        // whether the press point read back from the server was inside.
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

        // Park, approach the field, click it — that click is what takes
        // keyboard focus for everything typed below.
        auto park_x = static_cast<std::int16_t>( field->rect_.x_ );
        auto park_y = static_cast<std::int16_t>( field->rect_.y_ );
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

        const bool field_click_inside = approach_and_click( *field );
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::CursorPosition,
                                            .subject_ = field_subject,
                                            .value_ = field_click_inside
                                                          ? "inside"
                                                          : "outside" } );
        std::this_thread::sleep_for( std::chrono::milliseconds{ settle_ms } );

        // ── 5. TYPE, letter by letter ───────────────────────────────────────
        const std::string_view text{ paragraph };
        std::size_t            typed = 0U;
        for( std::size_t index = 0U; index < text.size(); ++index )
        {
            const char one[2] = { text[index], '\0' };
            if( auto sent = ( *input ).type_text( one ); sent.has_value() )
            {
                ++typed;
            }
            double gap_ms =
                std::exp( rng.normal( key_gap_ln_mean, key_gap_ln_sigma ) );
            const char just = text[index];
            if( just == ' ' )
            {
                gap_ms += rng.uniform( word_gap_ms * 0.6, word_gap_ms * 1.4 );
            }
            else if( just == ',' )
            {
                gap_ms += rng.uniform( comma_gap_ms * 0.7, comma_gap_ms * 1.3 );
            }
            else if( just == '.' )
            {
                gap_ms += rng.uniform( stop_gap_ms * 0.7, stop_gap_ms * 1.3 );
            }
            else if( rng.uniform() < think_chance )
            {
                gap_ms += rng.uniform( think_gap_ms * 0.6, think_gap_ms * 1.4 );
            }
            std::this_thread::sleep_for(
                std::chrono::duration<double, std::milli>( gap_ms )
            );
        }
        std::cout << "  typed     " << typed << "/" << text.size()
                  << " characters, letter by letter\n";
        std::this_thread::sleep_for( std::chrono::milliseconds{ react_ms } );

        // The field's value, read back through the tree BEFORE send.
        std::string value_seen = "(unresolved)";
        for( int attempt = 0; attempt < poll_tries; ++attempt )
        {
            if( auto again = resolve_role_named( **session, grab::role::entry,
                                                 { field_label } );
                again.has_value() )
            {
                value_seen = again->text_;
                if( value_seen == text )
                {
                    break;
                }
            }
            std::this_thread::sleep_for( std::chrono::milliseconds{ poll_ms } );
        }
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::A11yValue,
                                            .subject_ = field_subject,
                                            .value_   = value_seen } );
        std::cout << "  a11y      value "
                  << ( value_seen == text ? "matches, exactly"
                                          : "MISMATCH: \"" + value_seen + "\"" )
                  << '\n';

        auto typed_frame = ( *screen ).display();
        if( typed_frame.has_value() )
        {
            pixel::write_ppm( *typed_frame, options.out / "02-typed.ppm" );
        }

        // ── 6. SEND ─────────────────────────────────────────────────────────
        const bool send_click_inside = approach_and_click( *send );
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::CursorPosition,
                                            .subject_ = send_subject,
                                            .value_ = send_click_inside ? "inside"
                                                                        : "outside" } );
        std::this_thread::sleep_for( std::chrono::milliseconds{ react_ms } );

        std::string send_after = "(unresolved)";
        for( int attempt = 0; attempt < poll_tries; ++attempt )
        {
            if( auto again = resolve_role_named( **session, grab::role::button,
                                                 { send_done, send_idle } );
                again.has_value() )
            {
                send_after = again->name_;
                if( send_after == send_done )
                {
                    break;
                }
            }
            std::this_thread::sleep_for( std::chrono::milliseconds{ poll_ms } );
        }
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::A11yName,
                                            .subject_ = send_subject,
                                            .value_   = send_after } );

        // The receipt: what the PAGE's own script says it received.
        const std::string expected_receipt = std::string{ receipt_prefix } + paragraph;
        std::string       receipt_seen     = "(unresolved)";
        if( auto receipt = resolve_role_named(
                **session, grab::role::button,
                { std::string_view{ expected_receipt }, receipt_empty } );
            receipt.has_value() )
        {
            receipt_seen = receipt->name_;
        }
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::A11yName,
                                            .subject_ = receipt_subject,
                                            .value_   = receipt_seen } );
        std::cout << "  send      \"" << send_after << "\", receipt "
                  << ( receipt_seen == expected_receipt ? "carries the exact text"
                                                        : "MISMATCH" )
                  << '\n';

        std::this_thread::sleep_for( std::chrono::milliseconds{ settle_ms } );
        auto        after   = ( *screen ).display();
        std::string flipped = "unreadable";
        if( typed_frame.has_value() && after.has_value() )
        {
            const auto was = pixel::mean_colour( *typed_frame, send->rect_ );
            const auto now = pixel::mean_colour( *after, send->rect_ );
            if( was.has_value() && now.has_value() )
            {
                const double moved = pixel::distance( *was, *now );
                flipped            = moved >= colour_match ? "changed" : "unchanged";
                std::cout << "  pixel     send mean colour moved "
                          << static_cast<int>( moved ) << " (>= "
                          << static_cast<int>( colour_match )
                          << " counts as changed)\n";
            }
        }
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::PixelColour,
                                            .subject_ = send_subject,
                                            .value_   = flipped } );
        if( after.has_value() )
        {
            pixel::write_ppm( *after, options.out / "03-after.ppm" );
        }

        // The device channel, drained once: at least one key-down per typed
        // character, and a click pair inside the send button.
        std::string keys_seen       = "not observed";
        std::string send_click_seen = "not observed";
        if( event_subscription.has_value() )
        {
            std::size_t                     key_downs = 0U;
            std::optional<grab::SpacePoint> down_pos;
            std::optional<grab::SpacePoint> up_pos;
            bool                            send_pair = false;
            while( auto event = event_subscription->try_pop() )
            {
                if( event->kind == grab::EventKind::KeyDown )
                {
                    ++key_downs;
                    continue;
                }
                const auto* mb = std::get_if<grab::MouseButton>( &event->payload );
                if( mb == nullptr || mb->button != primary_button )
                {
                    continue;
                }
                if( event->kind == grab::EventKind::MouseButtonDown &&
                    mb->position.has_value() )
                {
                    down_pos = mb->position;
                }
                else if( event->kind == grab::EventKind::MouseButtonUp &&
                         mb->position.has_value() )
                {
                    up_pos = mb->position;
                    if( down_pos.has_value() &&
                        send->rect_.contains( down_pos->x, down_pos->y ) &&
                        send->rect_.contains( up_pos->x, up_pos->y ) )
                    {
                        send_pair = true;
                    }
                }
            }
            if( key_downs >= text.size() )
            {
                keys_seen = "typed";
            }
            if( send_pair )
            {
                send_click_seen = "clicked";
            }
            std::cout << "  events    " << key_downs << " key-down(s) for "
                      << text.size() << " characters, send click "
                      << send_click_seen << '\n';
        }
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::ButtonClick,
                                            .subject_ = keys_subject,
                                            .value_   = keys_seen } );
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::ButtonClick,
                                            .subject_ = send_subject,
                                            .value_   = send_click_seen } );

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
