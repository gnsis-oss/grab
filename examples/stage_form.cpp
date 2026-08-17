// ┌──────────────────────────────────────────────────────────────────────────┐
// │  stage_form — rung 7 of the capability ladder: the controls.             │
// │                                                                          │
// │  ONE page carrying EVERY HTML input type — all 22 of them — plus a       │
// │  combo box. The interesting ones are driven for real:                    │
// │                                                                          │
// │    text      click, type "grab"                                          │
// │    checkbox  click it on                                                 │
// │    radio     click option B of a group                                   │
// │    combo     click to open the popup, then click the wanted option in   │
// │              it — BY MOUSE, resolving the option's live popup rect       │
// │    range     click the track at 80% — the slider jumps to the pointer    │
// │    number    click, type "42"                                            │
// │    date      click, type the digits of a date into its segments          │
// │    password  click, type a secret                                        │
// │    APPLY     a real type=submit, clicked last                            │
// │                                                                          │
// │  Verification never trusts this program's own bookkeeping: every         │
// │  control mirrors its value into a page element whose accessible name     │
// │  is read back through AT-SPI; APPLY builds a SUMMARY of everything the   │
// │  page's script received; a catalog element lists the input types the     │
// │  DOM actually contains, which must be all 22; and the X server's own    │
// │  event stream must carry every click pair and a key-down per typed      │
// │  character.                                                              │
// │                                                                          │
// │  Aiming is by AUTHORED geometry plus the measured chrome: exotic input   │
// │  types have no stable a11y role to resolve by, but the markup and the    │
// │  truth come from one place, so the click targets do too.                 │
// │                                                                          │
// │    stage_form                 headless, on a display it creates          │
// │    stage_form --session       the display you are already on            │
// │    stage_form --trail         approaches drawn on the overlay           │
// │    stage_form --trail --watch same, in a Xephyr WINDOW you can watch     │
// │    stage_form --keep          leave the session up afterwards           │
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

    constexpr int viewport_w = 1'000;
    constexpr int viewport_h = 700;

    // Driven controls, PAGE coordinates. Everything fits one screenful.
    constexpr view::ViewRect text_rect{ .x_ = 20, .y_ = 20, .w_ = 280, .h_ = 40 };
    constexpr view::ViewRect check_rect{ .x_ = 20, .y_ = 80, .w_ = 32, .h_ = 32 };
    constexpr view::ViewRect radio_a_rect{ .x_ = 20, .y_ = 130, .w_ = 32, .h_ = 32 };
    constexpr view::ViewRect radio_b_rect{ .x_ = 90, .y_ = 130, .w_ = 32, .h_ = 32 };
    constexpr view::ViewRect combo_rect{ .x_ = 20, .y_ = 180, .w_ = 220, .h_ = 40 };
    constexpr view::ViewRect range_rect{ .x_ = 20, .y_ = 240, .w_ = 280, .h_ = 30 };
    constexpr view::ViewRect number_rect{ .x_ = 20, .y_ = 300, .w_ = 140, .h_ = 40 };
    constexpr view::ViewRect date_rect{ .x_ = 20, .y_ = 360, .w_ = 220, .h_ = 40 };
    constexpr view::ViewRect pass_rect{ .x_ = 20, .y_ = 420, .w_ = 220, .h_ = 40 };
    constexpr view::ViewRect apply_rect{ .x_ = 860, .y_ = 340, .w_ = 120, .h_ = 56 };
    // The receipt doubles as the calibration's SECOND anchor — keep in sync
    // with the literal geometry page_html() writes for #receipt.
    constexpr view::ViewRect receipt_rect{ .x_ = 540, .y_ = 420, .w_ = 440, .h_ = 50 };

    // Where on the range track to click, as a fraction of its width. The
    // slider jumps to the pointer, so the landed value is this fraction of
    // the scale give or take the track's end padding — asserted as a RANGE,
    // never as an exact number.
    constexpr double range_click_fraction = 0.80;
    constexpr double range_low            = 70.0;
    constexpr double range_high           = 90.0;

    constexpr const char* typed_text   = "grab";
    constexpr const char* typed_number = "42";
    // The date's digits in the order Firefox's en-US segments consume them
    // (mm dd yyyy), and the ISO value the page must end up holding.
    constexpr const char* typed_date   = "08172026";
    constexpr const char* iso_date     = "2026-08-17";
    constexpr const char* typed_secret = "hunter2";

    constexpr const char* apply_idle = "APPLY";
    constexpr const char* apply_done = "APPLIED";

    // Every HTML input type, alphabetically — the page's own script
    // enumerates what the DOM actually contains and must reproduce this
    // list exactly.
    constexpr const char* all_input_types =
        "button checkbox color date datetime-local email file hidden image "
        "month number password radio range reset search submit tel text "
        "time url week";

    constexpr const char*   title_marker = "Stage Form";

    constexpr std::uint32_t primary_button = 1U;    // X11 primary pointer button

    constexpr grab::overlay::Color cyan{ .r = 0U, .g = 217U, .b = 255U, .a = 242U };
    constexpr grab::overlay::Color amber{ .r = 255U, .g = 184U, .b = 26U, .a = 242U };

    constexpr double               stroke_px       = 3.0;
    constexpr double               trail_stroke_px = 2.0;
    constexpr auto                 trail_slack = std::chrono::milliseconds{ 8 };

    constexpr double        park_fx = 0.08;
    constexpr double        park_fy = 0.88;

    // Typing rhythm — same family as stage_type, shorter strings.
    constexpr double        key_gap_ln_mean  = 4.01;    // ln(55 ms)
    constexpr double        key_gap_ln_sigma = 0.35;

    constexpr std::uint64_t seed        = 0X5'D1'DE'00'07ULL;
    constexpr int           settle_ms   = 700;
    constexpr int           announce_ms = 550;
    constexpr int           react_ms    = 450;
    constexpr int           poll_ms     = 200;
    constexpr int    poll_tries   = 150;    // 30 s: Firefox builds its a11y tree lazily
    constexpr double colour_match = 40.0;

    [[nodiscard]]
    std::string
    px_rect( const view::ViewRect& rect )
    {
        return "left:" + std::to_string( static_cast<int>( rect.x_ ) ) +
               "px;top:" + std::to_string( static_cast<int>( rect.y_ ) ) +
               "px;width:" + std::to_string( static_cast<int>( rect.w_ ) ) +
               "px;height:" + std::to_string( static_cast<int>( rect.h_ ) ) + "px;";
    }

    [[nodiscard]]
    std::string
    page_html()
    {
        // The catalog column: one small instance of every input type the
        // driven form does not already carry. A 1x1 transparent gif keeps
        // type=image from rendering as a broken-image icon.
        constexpr const char* pixel_gif =
            "data:image/gif;base64,R0lGODlhAQABAIAAAP///wAAACH5BAEAAAAALAAAAAABAAEAAAICRAEAOw==";
        const std::string     catalog_types[] = { "button", "color",  "datetime-local",
                                                  "email",  "file",   "hidden",
                                                  "image",  "month",  "reset",
                                                  "search", "tel",    "time",
                                                  "url",    "week" };
        std::string           catalog;
        int                   slot = 0;
        for( const std::string& type : catalog_types )
        {
            const int col = slot % 2;
            const int row = slot / 2;
            ++slot;
            const int left = 540 + ( col * 190 );
            const int top  = 20 + ( row * 44 );
            catalog += "<input type=\"" + type + "\"";
            if( type == "image" )
            {
                catalog += " src=\"" + std::string{ pixel_gif } + "\"";
            }
            catalog += " style=\"position:absolute;left:" + std::to_string( left ) +
                       "px;top:" + std::to_string( top ) +
                       "px;width:150px;\" tabindex=\"-1\">\n";
        }

        const auto mirror = [&]( const char* id, int top )
        {
            return std::string{ "<button id=\"" } + id +
                   "\" aria-label=\"" + id +
                   " EMPTY\" tabindex=\"-1\" style=\"position:absolute;"
                   "left:320px;top:" +
                   std::to_string( top ) +
                   "px;width:200px;height:30px;background:#eef1f6;"
                   "border:1px solid #b7c3d6;font:400 12px sans-serif;"
                   "color:#33415c;\">" +
                   id + " EMPTY</button>\n";
        };

        return std::string{ "<!doctype html>\n"
                            "<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
                            "<title>Stage Form — grab</title>\n"
                            "<style>\n"
                            "  html,body{margin:0;padding:0;background:#f4f4f4;"
                            "font:400 16px sans-serif;}\n"
                            "  input,select{box-sizing:border-box;}\n"
                            "</style></head><body>\n"
                            "<form id=\"f\">\n" } +
               "<input type=\"text\" id=\"f_text\" aria-label=\"TEXT\" "
               "style=\"position:absolute;" + px_rect( text_rect ) + "\">\n"
               "<input type=\"checkbox\" id=\"f_check\" aria-label=\"CHECK\" "
               "style=\"position:absolute;" + px_rect( check_rect ) + "\">\n"
               "<input type=\"radio\" name=\"r\" id=\"f_radio_a\" aria-label=\"RADIO "
               "A\" style=\"position:absolute;" + px_rect( radio_a_rect ) + "\">\n"
               "<input type=\"radio\" name=\"r\" id=\"f_radio_b\" aria-label=\"RADIO "
               "B\" style=\"position:absolute;" + px_rect( radio_b_rect ) + "\">\n"
               "<select id=\"f_combo\" aria-label=\"COMBO\" "
               "style=\"position:absolute;" + px_rect( combo_rect ) + "\">\n"
               "<option>alpha</option><option>beta</option><option>gamma</option>\n"
               "</select>\n"
               "<input type=\"range\" id=\"f_range\" aria-label=\"RANGE\" min=\"0\" "
               "max=\"100\" step=\"1\" value=\"50\" "
               "style=\"position:absolute;" + px_rect( range_rect ) + "\">\n"
               "<input type=\"number\" id=\"f_number\" aria-label=\"NUMBER\" "
               "style=\"position:absolute;" + px_rect( number_rect ) + "\">\n"
               "<input type=\"date\" id=\"f_date\" aria-label=\"DATE\" "
               "style=\"position:absolute;" + px_rect( date_rect ) + "\">\n"
               "<input type=\"password\" id=\"f_pass\" aria-label=\"PASS\" "
               "style=\"position:absolute;" + px_rect( pass_rect ) + "\">\n"
               "<input type=\"submit\" id=\"f_apply\" aria-label=\"" + apply_idle +
               "\" value=\"" + apply_idle + "\" style=\"position:absolute;" +
               px_rect( apply_rect ) +
               "background:#1d4e89;color:#fff;border:0;"
               "font:600 22px sans-serif;\">\n"
               "</form>\n" +
               catalog +
               mirror( "m_text", 20 ) + mirror( "m_check", 80 ) +
               mirror( "m_radio", 130 ) + mirror( "m_combo", 180 ) +
               mirror( "m_range", 240 ) + mirror( "m_number", 300 ) +
               mirror( "m_date", 360 ) + mirror( "m_pass", 420 ) +
               "<button id=\"receipt\" aria-label=\"SUMMARY EMPTY\" tabindex=\"-1\" "
               "style=\"position:absolute;left:540px;top:420px;width:440px;"
               "height:50px;background:#e8e8e8;border:2px dashed #8a8a8a;"
               "color:#555;font:400 11px sans-serif;\">SUMMARY EMPTY</button>\n"
               "<button id=\"catalog\" aria-label=\"CATALOG EMPTY\" tabindex=\"-1\" "
               "style=\"position:absolute;left:540px;top:490px;width:440px;"
               "height:40px;background:#e8e8e8;border:1px solid #b7c3d6;"
               "color:#555;font:400 10px sans-serif;\">CATALOG EMPTY</button>\n"
               "<script>\n"
               "  var $=function(i){return document.getElementById(i);};\n"
               "  var set=function(i,v){var e=$(i);"
               "e.setAttribute('aria-label',v);e.textContent=v;};\n"
               "  var t=$('f_text'),c=$('f_check'),ra=$('f_radio_a'),"
               "rb=$('f_radio_b'),s=$('f_combo'),r=$('f_range'),n=$('f_number'),"
               "d=$('f_date'),p=$('f_pass');\n"
               "  t.addEventListener('input',function(){set('m_text','m_text "
               "'+t.value);});\n"
               "  c.addEventListener('change',function(){set('m_check','m_check "
               "'+(c.checked?'on':'off'));});\n"
               "  var radio=function(){set('m_radio','m_radio "
               "'+(rb.checked?'b':(ra.checked?'a':'none')));};\n"
               "  ra.addEventListener('change',radio);\n"
               "  rb.addEventListener('change',radio);\n"
               "  var combo=function(){set('m_combo','m_combo '+s.value);};\n"
               "  s.addEventListener('input',combo);\n"
               "  s.addEventListener('change',combo);\n"
               "  r.addEventListener('input',function(){set('m_range',"
               "String(r.value));});\n"
               "  n.addEventListener('input',function(){set('m_number','m_number "
               "'+n.value);});\n"
               "  d.addEventListener('input',function(){set('m_date','m_date "
               "'+d.value);});\n"
               "  p.addEventListener('input',function(){set('m_pass','m_pass "
               "'+p.value.length);});\n"
               "  $('f').addEventListener('submit',function(e){\n"
               "    e.preventDefault();\n"
               "    var a=$('f_apply');\n"
               "    a.style.background='#2a9d3a';\n"
               "    a.setAttribute('aria-label','" + apply_done + "');\n"
               "    a.value='" + apply_done + "';\n"
               "    set('receipt','SUMMARY text='+t.value+' check='+"
               "(c.checked?'on':'off')+' radio='+(rb.checked?'b':(ra.checked?'a':"
               "'none'))+' combo='+s.value+' range='+r.value+' number='+n.value+"
               "' date='+d.value+' pw='+p.value.length);\n"
               "  });\n"
               "  // The AUTHORED type attribute, not the .type property: a\n"
               "  // browser that does not implement a type (Firefox lacks\n"
               "  // month and week) normalises the property to 'text', and\n"
               "  // the claim here is about what the page CARRIES.\n"
               "  var types=[];\n"
               "  document.querySelectorAll('input').forEach(function(i){\n"
               "    var t=i.getAttribute('type');\n"
               "    if(t&&types.indexOf(t)<0){types.push(t);}\n"
               "  });\n"
               "  types.sort();\n"
               "  set('catalog','CATALOG '+types.join(' '));\n"
               "</script></body></html>\n";
    }

    [[nodiscard]]
    stage::Scene
    build_scene()
    {
        stage::Scene scene;
        scene.id_ = "form";
        scene.pages_.push_back( stage::ScenePage{ .name_   = "form",
                                                  .html_   = page_html(),
                                                  .marker_ = title_marker } );
        scene.viewport_ = stage::ViewportSpec{ .viewport_w_ = viewport_w,
                                               .viewport_h_ = viewport_h,
                                               .document_h_ = viewport_h };
        scene.frames_   = { "01-empty", "02-filled", "03-after" };

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
        expect( "catalog_carries_all_types",
                stage::Observe::A11yName,
                "catalog",
                std::string{ "CATALOG " } + all_input_types );
        expect( "text_mirrored",
                stage::Observe::A11yName,
                "m_text",
                std::string{ "m_text " } + typed_text );
        expect( "checkbox_mirrored", stage::Observe::A11yName, "m_check",
                "m_check on" );
        expect( "radio_mirrored", stage::Observe::A11yName, "m_radio", "m_radio b" );
        expect( "combo_mirrored", stage::Observe::A11yName, "m_combo",
                "m_combo beta" );
        // The range value is a RANGE, never an exact number: the slider
        // jumps to the pointer, and the track's end padding shifts the
        // landed value by a few units.
        scene.expect_.push_back( stage::Expectation{ .name_ = "range_in_band",
                                                     .observe_ =
                                                         stage::Observe::A11yValue,
                                                     .subject_   = "m_range",
                                                     .value_     = "",
                                                     .tolerance_ = 0.0,
                                                     .low_       = range_low,
                                                     .high_      = range_high,
                                                     .ranged_    = true } );
        expect( "number_mirrored",
                stage::Observe::A11yName,
                "m_number",
                std::string{ "m_number " } + typed_number );
        expect( "date_mirrored",
                stage::Observe::A11yName,
                "m_date",
                std::string{ "m_date " } + iso_date );
        expect( "password_length_mirrored", stage::Observe::A11yName, "m_pass",
                "m_pass 7" );
        expect( "apply_reports_applied",
                stage::Observe::A11yName,
                "apply",
                apply_done );
        // summary_matches is declared at run time once the landed range value
        // is known — see the SUMMARY section below, which appends it to a
        // COPY of this scene before scoring. Declared here as a named
        // placeholder so an act that skips the summary still fails loudly.
        expect( "presses_inside", stage::Observe::CursorPosition, "presses",
                "inside" );
        expect( "keystrokes_observed", stage::Observe::ButtonClick, "keys", "typed" );
        expect( "click_pairs_observed",
                stage::Observe::ButtonClick,
                "clicks",
                "all observed" );
        expect( "apply_pixels_flipped", stage::Observe::PixelColour, "apply",
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

    // Resolve the document node of `role_id` whose accessible name STARTS
    // WITH `prefix`. Mirrors carry their value in the name, so the exact
    // name is unknown until read — the prefix is the identity.
    [[nodiscard]]
    std::optional<Live>
    resolve_prefix( grab::Session&   session,
                    grab::RoleId     role_id,
                    std::string_view prefix )
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
            if( info.name.starts_with( prefix ) )
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
        return std::nullopt;
    }

    struct Options
    {
            std::string           display = ":67";
            std::filesystem::path out     = "stage-form";
            std::string           host_display;
            bool                  attach = false;
            bool                  keep   = false;
            bool                  trail  = false;
    };

    void
    usage()
    {
        std::cout << "stage_form — rung 7: every input type, the useful ones driven\n\n"
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
                std::cerr << "stage_form: " << flag << ' ' << reason << "\n\n";
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
                    return bad( "requires a display, e.g. :67" );
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
                    std::cerr << "stage_form: --session needs DISPLAY set\n";
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
            std::cerr << "stage_form: --watch and --session are mutually exclusive\n\n";
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
    const Options options = *parsed;

    stage::Scene  scene   = build_scene();
    stage::Observations seen;

    // ── 1. AUTHOR ───────────────────────────────────────────────────────────
    std::error_code     ignored;
    std::filesystem::create_directories( options.out, ignored );
    const std::filesystem::path page = options.out / "form.html";
    {
        std::ofstream out( page );
        out << scene.pages_.front().html_;
    }
    std::cout << "AUTHOR\n  form      9 driven controls + a 22-type catalog -> "
              << page << '\n';

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

        // ── 3. RESOLVE + CALIBRATE ──────────────────────────────────────────
        // APPLY is the calibration anchor: a plain button with an authored
        // rect, resolvable by role. Its screen position against its page
        // position measures the chrome; every other control is then aimed
        // at by authored geometry alone — exotic input types have no stable
        // a11y role to resolve by.
        std::optional<Live> apply;
        for( int attempt = 0; attempt < poll_tries && !apply.has_value(); ++attempt )
        {
            apply = resolve_prefix( **session, grab::role::button, apply_idle );
            if( !apply.has_value() )
            {
                std::this_thread::sleep_for( std::chrono::milliseconds{ poll_ms } );
            }
        }
        if( !apply.has_value() )
        {
            std::cerr << "the APPLY button never appeared in the a11y tree\n";
            host.stop();
            return 1;
        }
        const auto summary_window = host.browser_window();
        if( !summary_window.has_value() )
        {
            std::cerr << "the browser window disappeared from the window list\n";
            host.stop();
            return 1;
        }
        const view::ViewRect window_rect{
            .x_ = static_cast<double>( summary_window->bounds.x ),
            .y_ = static_cast<double>( summary_window->bounds.y ),
            .w_ = static_cast<double>( summary_window->bounds.width ),
            .h_ = static_cast<double>( summary_window->bounds.height ),
        };
        std::cout << "  window    (" << summary_window->bounds.x << ","
                  << summary_window->bounds.y << " " << summary_window->bounds.width
                  << "x" << summary_window->bounds.height << ")\n";

        // ── Calibration, ENTIRELY in a11y space ─────────────────────────────
        //
        // The first version measured a chrome offset by mixing the a11y rect
        // with the WM-reported window frame, assuming the two differ by a
        // pure translation. On a real GNOME desktop they do not — CSD
        // shadows and scaling put the frame in a space of its own — and the
        // run walked the pointer over the operator's live desktop on a
        // garbage transform. The rungs that aim by raw a11y rects work on
        // that same desktop, so a11y space is the one INPUT provably lands
        // in; the window frame is never mixed into aiming again.
        //
        // APPLY's resolved rect gives the whole affine map: its live size
        // against the authored 120x56 is the scale, its live position minus
        // the scaled authored position is the origin.
        const double scale_x  = apply->rect_.w_ / apply_rect.w_;
        const double scale_y  = apply->rect_.h_ / apply_rect.h_;
        const double origin_x = apply->rect_.x_ - ( scale_x * apply_rect.x_ );
        const double origin_y = apply->rect_.y_ - ( scale_y * apply_rect.y_ );
        std::cout << "  calibrate scale (" << scale_x << "," << scale_y
                  << ")  origin (" << static_cast<int>( origin_x ) << ","
                  << static_cast<int>( origin_y ) << ")  from APPLY\n";

        const auto to_screen = [&]( const view::ViewRect& rect ) -> view::ViewRect
        {
            return view::ViewRect{ .x_ = origin_x + ( scale_x * rect.x_ ),
                                   .y_ = origin_y + ( scale_y * rect.y_ ),
                                   .w_ = scale_x * rect.w_,
                                   .h_ = scale_y * rect.h_ };
        };

        // The gate: a calibration is BELIEVED only after it predicts a
        // SECOND anchor. The receipt button is authored at the other end of
        // the page; resolve it, predict its centre through the map, and
        // refuse to synthesize a single click if the prediction misses.
        // Clicking a live desktop on an unverified transform is what this
        // example did once, and never does again.
        constexpr double sane_scale_low   = 0.5;
        constexpr double sane_scale_high  = 3.0;
        constexpr double anchor_slack_px  = 25.0;
        bool             calibration_sane = scale_x >= sane_scale_low &&
                                scale_x <= sane_scale_high &&
                                scale_y >= sane_scale_low &&
                                scale_y <= sane_scale_high;
        if( calibration_sane )
        {
            const auto receipt_anchor =
                resolve_prefix( **session, grab::role::button, "SUMMARY" );
            if( !receipt_anchor.has_value() )
            {
                calibration_sane = false;
                std::cerr << "calibration: the second anchor (receipt) never "
                             "resolved\n";
            }
            else
            {
                const view::ViewRect predicted = to_screen( receipt_rect );
                const double miss_x =
                    predicted.center_x() - receipt_anchor->rect_.center_x();
                const double miss_y =
                    predicted.center_y() - receipt_anchor->rect_.center_y();
                const double miss =
                    std::sqrt( ( miss_x * miss_x ) + ( miss_y * miss_y ) );
                std::cout << "  verify    second anchor off by "
                          << static_cast<int>( miss ) << " px (<= "
                          << static_cast<int>( anchor_slack_px )
                          << " arms the run)\n";
                calibration_sane = miss <= anchor_slack_px;
            }
        }
        if( !calibration_sane )
        {
            std::cerr << "calibration is not trustworthy — REFUSING to click "
                         "anything on this display.\n"
                         "scale ("
                      << scale_x << "," << scale_y
                      << ") from APPLY; see the numbers above.\n";
            host.stop();
            return 1;
        }

        auto empty_frame = ( *screen ).display();
        if( empty_frame.has_value() )
        {
            pixel::write_ppm( *empty_frame, options.out / "01-empty.ppm" );
        }

        // Where do overlay shapes ACTUALLY land? Measured, at APPLY.
        const auto omap = ladder::view::align::measure(
            overlay, *screen, space, apply->rect_.x_, apply->rect_.y_ );

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

        bool all_presses_inside = true;

        // Approach `aim` (a small spot within `target`) and click. The
        // press-inside verdict is against the CONTROL's whole rect.
        const auto click_at =
            [&]( const view::ViewRect& target, const view::ViewRect& aim )
        {
            if( overlay != nullptr )
            {
                ( void )overlay->add( grab::overlay::Shape{
                    .geometry = grab::overlay::Rect{
                        .bounds = grab::SpaceRect{ .x     = omap.x( target.x_ ),
                                                   .y     = omap.y( target.y_ ),
                                                   .w     = target.w_ / omap.sx_,
                                                   .h     = target.h_ / omap.sy_,
                                                   .space = space } },
                    .stroke   = grab::overlay::StrokeStyle{ .color    = cyan,
                                                            .width_px = stroke_px },
                    .fill     = std::nullopt,
                    .lifetime =
                        grab::overlay::Ttl{
                            .duration = std::chrono::milliseconds{ 1'200 } },
                    .band = grab::overlay::Band::Annotation,
                    .z    = 0,
                } );
                ( void )overlay->flush();
            }
            std::this_thread::sleep_for( std::chrono::milliseconds{ announce_ms } );

            const auto   at = ( *input ).position();
            motion::Vec2 cursor{ at.has_value() ? static_cast<double>( at->x )
                                                : target.x_,
                                 at.has_value() ? static_cast<double>( at->y )
                                                : target.y_ };
            const motion::Rect aim_rect{ aim.x_, aim.y_, aim.w_, aim.h_ };
            const auto         movement =
                motion::plan_move( rng, cursor, aim_rect, motion::MotionConfig{} );
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
            if( !at_press.has_value() ||
                !target.contains( static_cast<double>( at_press->x ),
                                  static_cast<double>( at_press->y ) ) )
            {
                all_presses_inside = false;
            }
            const double hold_s = std::exp( rng.normal( std::log( 0.080 ), 0.35 ) );
            ( void )( *input ).press();
            std::this_thread::sleep_for( std::chrono::duration<double>( hold_s ) );
            ( void )( *input ).release();
            retire_trail();
            std::this_thread::sleep_for( std::chrono::milliseconds{ react_ms } );
        };
        const auto click_center = [&]( const view::ViewRect& page_rect )
        {
            const view::ViewRect target = to_screen( page_rect );
            const view::ViewRect aim{ .x_ = target.center_x() - 4.0,
                                      .y_ = target.center_y() - 4.0,
                                      .w_ = 8.0,
                                      .h_ = 8.0 };
            click_at( target, aim );
        };

        std::size_t total_typed = 0U;
        const auto  type_string = [&]( std::string_view text )
        {
            for( const char letter : text )
            {
                const char one[2] = { letter, '\0' };
                if( auto sent = ( *input ).type_text( one ); sent.has_value() )
                {
                    ++total_typed;
                }
                std::this_thread::sleep_for(
                    std::chrono::duration<double, std::milli>( std::exp(
                        rng.normal( key_gap_ln_mean, key_gap_ln_sigma ) ) )
                );
            }
        };

        // Park, then run the form top to bottom.
        ( void )( *input ).move(
            static_cast<std::int16_t>( window_rect.x_ +
                                       ( window_rect.w_ * park_fx ) ),
            static_cast<std::int16_t>( window_rect.y_ +
                                       ( window_rect.h_ * park_fy ) )
        );
        std::this_thread::sleep_for( std::chrono::milliseconds{ settle_ms } );

        std::cout << "  text      click, type \"" << typed_text << "\"\n";
        click_center( text_rect );
        type_string( typed_text );

        std::cout << "  checkbox  click\n";
        click_center( check_rect );

        std::cout << "  radio     click option B\n";
        click_center( radio_b_rect );

        // The combo, BY MOUSE: click the select — the popup opens — then
        // resolve the wanted option's LIVE rect through the tree and click
        // the option itself. The popup is a transient surface, so the
        // option is searched without the document scope and across the
        // roles a dropdown option can surface as. Only if the popup never
        // exposes a clickable rect does the keyboard fallback run, loudly.
        std::cout << "  combo     click to open, then click \"beta\" in the "
                     "popup\n";
        click_center( combo_rect );
        std::this_thread::sleep_for( std::chrono::milliseconds{ react_ms } );
        std::optional<Live> option;
        for( int attempt = 0; attempt < 10 && !option.has_value(); ++attempt )
        {
            if( auto synced = ( *session )->resync(); synced.has_value() )
            {
                for( const grab::RoleId role_id :
                     { grab::role::control, grab::role::menu_item,
                       grab::role::list, grab::role::text } )
                {
                    auto matches = ( *session )->resolve_all(
                        grab::sel::role( role_id ) );
                    if( !matches.has_value() )
                    {
                        continue;
                    }
                    for( const grab::Match& match : *matches )
                    {
                        auto described = ( *session )->describe( match );
                        if( !described.has_value() )
                        {
                            continue;
                        }
                        const auto& info = *described;
                        // A popup row: named like the option, option-sized,
                        // and NOT the collapsed select itself.
                        if( info.name == "beta" && info.bounds.h >= 10.0 &&
                            info.bounds.h <= 60.0 && info.bounds.w >= 40.0 &&
                            info.bounds.w <= 500.0 )
                        {
                            option = Live{
                                .name_ = info.name,
                                .rect_ =
                                    view::ViewRect{ .x_ = info.bounds.x,
                                                    .y_ = info.bounds.y,
                                                    .w_ = info.bounds.w,
                                                    .h_ = info.bounds.h },
                                .space_ = info.bounds.space,
                            };
                            break;
                        }
                    }
                    if( option.has_value() )
                    {
                        break;
                    }
                }
            }
            if( !option.has_value() )
            {
                std::this_thread::sleep_for( std::chrono::milliseconds{ poll_ms } );
            }
        }
        if( option.has_value() )
        {
            std::cout << "  combo     option \"beta\" at ("
                      << static_cast<int>( option->rect_.x_ ) << ","
                      << static_cast<int>( option->rect_.y_ ) << " "
                      << static_cast<int>( option->rect_.w_ ) << "x"
                      << static_cast<int>( option->rect_.h_ )
                      << ") — clicking it\n";
            const view::ViewRect aim{ .x_ = option->rect_.center_x() - 4.0,
                                      .y_ = option->rect_.center_y() - 4.0,
                                      .w_ = 8.0,
                                      .h_ = 8.0 };
            click_at( option->rect_, aim );
        }
        else
        {
            std::cout << "  combo     popup exposed no clickable option rect — "
                         "FALLING BACK to keyboard (Escape, Down)\n";
            ( void )( *input ).press_key( "Escape" );
            std::this_thread::sleep_for( std::chrono::milliseconds{ react_ms } );
            ( void )( *input ).press_key( "Down" );
            std::this_thread::sleep_for( std::chrono::milliseconds{ react_ms } );
        }

        std::cout << "  range     click the track at "
                  << static_cast<int>( range_click_fraction * 100.0 ) << "%\n";
        {
            const view::ViewRect target = to_screen( range_rect );
            const view::ViewRect aim{
                .x_ = target.x_ + ( target.w_ * range_click_fraction ) - 3.0,
                .y_ = target.center_y() - 4.0,
                .w_ = 6.0,
                .h_ = 8.0 };
            click_at( target, aim );
        }

        std::cout << "  number    click, type \"" << typed_number << "\"\n";
        click_center( number_rect );
        type_string( typed_number );

        std::cout << "  date      click, type the segments of " << iso_date << '\n';
        click_center( date_rect );
        type_string( typed_date );

        std::cout << "  password  click, type " << std::string_view{ typed_secret }.size()
                  << " characters\n";
        click_center( pass_rect );
        type_string( typed_secret );

        std::this_thread::sleep_for( std::chrono::milliseconds{ settle_ms } );

        // ── 5. READ THE MIRRORS ─────────────────────────────────────────────
        const auto mirror_value =
            [&]( std::string_view id ) -> std::string
        {
            for( int attempt = 0; attempt < 10; ++attempt )
            {
                if( auto found = resolve_prefix( **session, grab::role::button, id );
                    found.has_value() && !found->name_.ends_with( "EMPTY" ) )
                {
                    return found->name_;
                }
                std::this_thread::sleep_for( std::chrono::milliseconds{ poll_ms } );
            }
            return "(unresolved)";
        };

        const std::array<const char*, 7U> mirror_ids{ "m_text",   "m_check",
                                                      "m_radio",  "m_combo",
                                                      "m_number", "m_date",
                                                      "m_pass" };
        for( const char* id : mirror_ids )
        {
            const std::string value = mirror_value( id );
            seen.push_back( stage::Observation{ .observe_ = stage::Observe::A11yName,
                                                .subject_ = id,
                                                .value_   = value } );
            std::cout << "  mirror    " << value << '\n';
        }
        // The range mirror is bare digits so the band check can parse it.
        std::string range_value = "(unresolved)";
        for( int attempt = 0; attempt < 10; ++attempt )
        {
            // The range mirror's name is JUST the number, so it is the only
            // all-digit button on the page.
            if( auto synced = ( *session )->resync(); synced.has_value() )
            {
                if( auto matches = ( *session )->resolve_all(
                        grab::sel::role( grab::role::button )
                            .and_( grab::sel::descendant_of(
                                grab::sel::role( grab::role::document ) ) ) );
                    matches.has_value() )
                {
                    for( const grab::Match& match : *matches )
                    {
                        auto described = ( *session )->describe( match );
                        if( !described.has_value() || described->name.empty() )
                        {
                            continue;
                        }
                        const std::string& name = described->name;
                        const bool         digits =
                            name.find_first_not_of( "0123456789" ) ==
                            std::string::npos;
                        if( digits )
                        {
                            range_value = name;
                            break;
                        }
                    }
                }
            }
            if( range_value != "(unresolved)" )
            {
                break;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds{ poll_ms } );
        }
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::A11yValue,
                                            .subject_ = "m_range",
                                            .value_   = range_value } );
        std::cout << "  mirror    m_range " << range_value << '\n';

        const std::string catalog_value = mirror_value( "CATALOG" );
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::A11yName,
                                            .subject_ = "catalog",
                                            .value_   = catalog_value } );

        auto filled_frame = ( *screen ).display();
        if( filled_frame.has_value() )
        {
            pixel::write_ppm( *filled_frame, options.out / "02-filled.ppm" );
        }

        // ── 6. APPLY ────────────────────────────────────────────────────────
        // The summary expectation can only be declared NOW: the landed range
        // value is measured, not authored. Declared before the click, so the
        // act still cannot write its own exam.
        scene.expect_.push_back( stage::Expectation{
            .name_    = "summary_matches",
            .observe_ = stage::Observe::A11yName,
            .subject_ = "receipt",
            .value_ = std::string{ "SUMMARY text=" } + typed_text + " check=on" +
                      " radio=b combo=beta range=" + range_value + " number=" +
                      typed_number + " date=" + iso_date + " pw=7",
            .tolerance_ = 0.0,
            .low_       = 0.0,
            .high_      = 0.0,
            .ranged_    = false } );

        std::cout << "  apply     click\n";
        click_center( apply_rect );

        std::string apply_after = "(unresolved)";
        for( int attempt = 0; attempt < poll_tries; ++attempt )
        {
            if( auto again = resolve_prefix( **session, grab::role::button,
                                             "APPL" );
                again.has_value() )
            {
                apply_after = again->name_;
                if( apply_after == apply_done )
                {
                    break;
                }
            }
            std::this_thread::sleep_for( std::chrono::milliseconds{ poll_ms } );
        }
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::A11yName,
                                            .subject_ = "apply",
                                            .value_   = apply_after } );

        const std::string receipt_value = mirror_value( "SUMMARY" );
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::A11yName,
                                            .subject_ = "receipt",
                                            .value_   = receipt_value } );
        std::cout << "  apply     \"" << apply_after << "\"\n  receipt   "
                  << receipt_value << '\n';

        std::this_thread::sleep_for( std::chrono::milliseconds{ settle_ms } );
        auto        after   = ( *screen ).display();
        std::string flipped = "unreadable";
        if( filled_frame.has_value() && after.has_value() )
        {
            const auto apply_screen = to_screen( apply_rect );
            const auto was = pixel::mean_colour( *filled_frame, apply_screen );
            const auto now = pixel::mean_colour( *after, apply_screen );
            if( was.has_value() && now.has_value() )
            {
                const double moved = pixel::distance( *was, *now );
                flipped            = moved >= colour_match ? "changed" : "unchanged";
                std::cout << "  pixel     apply mean colour moved "
                          << static_cast<int>( moved ) << " (>= "
                          << static_cast<int>( colour_match )
                          << " counts as changed)\n";
            }
        }
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::PixelColour,
                                            .subject_ = "apply",
                                            .value_   = flipped } );
        if( after.has_value() )
        {
            pixel::write_ppm( *after, options.out / "03-after.ppm" );
        }

        // ── 7. THE DEVICE CHANNEL, drained once ─────────────────────────────
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::CursorPosition,
                                            .subject_ = "presses",
                                            .value_ = all_presses_inside
                                                          ? "inside"
                                                          : "outside" } );
        std::string keys_seen   = "not observed";
        std::string clicks_seen = "not observed";
        if( event_subscription.has_value() )
        {
            std::size_t key_downs   = 0U;
            std::size_t click_pairs = 0U;
            std::optional<grab::SpacePoint> down_pos;
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
                if( event->kind == grab::EventKind::MouseButtonDown )
                {
                    down_pos = mb->position;
                }
                else if( event->kind == grab::EventKind::MouseButtonUp &&
                         down_pos.has_value() )
                {
                    ++click_pairs;
                    down_pos.reset();
                }
            }
            if( key_downs >= total_typed )
            {
                keys_seen = "typed";
            }
            // At least 9 driven clicks: text, checkbox, radio, combo (two
            // when the popup option is clicked by mouse), range, number,
            // date, password, apply.
            constexpr std::size_t driven_clicks = 9U;
            if( click_pairs >= driven_clicks )
            {
                clicks_seen = "all observed";
            }
            std::cout << "  events    " << key_downs << " key-down(s) for "
                      << total_typed << " typed, " << click_pairs
                      << " click pair(s) for " << driven_clicks << " clicks\n";
        }
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::ButtonClick,
                                            .subject_ = "keys",
                                            .value_   = keys_seen } );
        seen.push_back( stage::Observation{ .observe_ = stage::Observe::ButtonClick,
                                            .subject_ = "clicks",
                                            .value_   = clicks_seen } );

        // ── 8. SCORE ────────────────────────────────────────────────────────
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
