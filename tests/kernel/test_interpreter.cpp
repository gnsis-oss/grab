// The sequence loader: JSON bytes in, a typed Sequence out, and back again.
//
// The properties worth restating, because they are the ones easiest to break:
//
//   * A step with no `after` depends on the PRECEDING step in document order,
//     so a plain list reads top-to-bottom like the bash script it replaces.
//   * `id` is an author label, NEVER the identity. Every step gets a positional
//     StepId whether or not it carries one, which is why two byte-identical
//     unlabelled clicks are still different steps.
//   * Positional identity is also what makes parse -> to_json -> parse come
//     back identical WITHOUT ids ever being written into the document.
//   * `extra_grace_ms` under `strict` LOADS and is ignored. Rejecting it would
//     stop one document running under all three pacing modes, which is the
//     entire point of having modes.
//   * A shape carries EXACTLY ONE geometry key, and a shape with neither
//     stroke nor fill is legal — invisible, which is a useful thing to say.
//   * An overlay `handle` is a document-level name like a label: using one
//     before its `overlay.add`, or adding it again while it is still live, is
//     a LOADER error, while reusing one after a remove is not.

#include "grab/command.hpp"
#include "grab/command_descriptor.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "kernel/sequence/interpreter.hpp"
#include "kernel/sequence/sequence.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>
// clang-format on

namespace
{

    using grab::kernel::sequence::load;
    using grab::kernel::sequence::parse;
    using grab::kernel::sequence::Sequence;
    using grab::kernel::sequence::to_json;

    // ── Positions ────────────────────────────────────────────

    using Half                                = grab::sequence::StepId::Half;

    constexpr Half             firstIndex     = 0U;
    constexpr Half             secondIndex    = 1U;
    constexpr Half             thirdIndex     = 2U;
    constexpr Half             fourthIndex    = 3U;
    constexpr Half             fifthIndex     = 4U;
    constexpr Half             fifteenthIndex = 14U;

    // Where the eight overlay steps sit in everyOpDocument, which runs the 15
    // input, capture and wait ops first.
    constexpr Half             overlayUpdateIndex = 16U;
    constexpr Half             overlayAttachIndex = 17U;
    constexpr Half             overlayClearIndex  = 22U;

    constexpr std::size_t      noSteps            = 0U;
    constexpr std::size_t      oneEdge            = 1U;
    constexpr std::size_t      twoEdges           = 2U;
    constexpr std::size_t      twoSteps           = 2U;
    constexpr std::size_t      threeSteps         = 3U;
    constexpr std::size_t      fiveSteps          = 5U;
    constexpr std::size_t      graphFaultCount    = 5U;

    // How many ops everyOpDocument below covers — and it is an IDENTITY with
    // sequenceCommandCount, not a number that happens to match. Every
    // alternative of the Command variant is loadable from JSON, so an
    // alternative added with no parser fails here instead of passing quietly.
    constexpr std::size_t      everyOpStepCount = grab::sequence::sequenceCommandCount;

    // ── Payload values the assertions expect ─────────────────

    constexpr std::int32_t     warpX                     = 10;
    constexpr std::int32_t     warpY                     = 20;
    constexpr std::int64_t     waitMilliseconds          = 250;
    constexpr std::int64_t     extraGraceMilliseconds    = 400;
    constexpr std::int64_t     documentGraceMilliseconds = 80;
    constexpr std::uint8_t     leftButton                = 1U;
    constexpr std::uint8_t     middleButton              = 2U;
    constexpr std::uint8_t     rightButton               = 3U;
    constexpr std::uint8_t     wheelDownButton           = 5U;

    // ── Message fragments ────────────────────────────────────

    constexpr std::string_view unknownOpName     = "input.frobnicate";
    constexpr std::string_view unknownOpPhrase   = "unknown op";
    constexpr std::string_view unavailablePhrase = "is not available as a sequence step";
    constexpr std::string_view danglingPhrase    = "no step carries that label";
    constexpr std::string_view duplicatePhrase   = "duplicate step label";
    constexpr std::string_view cyclePhrase       = "cycle";
    constexpr std::string_view selfEdgePhrase    = "depends on itself";
    constexpr std::string_view repeatedPhrase    = "twice";
    constexpr std::string_view missingFilePhrase = "file not found";
    constexpr std::string_view unknownModePhrase = "unknown pacing mode";
    constexpr std::string_view opPointer         = "/steps/3/op";
    constexpr std::string_view danglingPointer   = "/steps/1/after/0";
    constexpr std::string_view duplicatePointer  = "/steps/1/id";
    constexpr std::string_view selfEdgePointer   = "/steps/0/after/0";
    constexpr std::string_view repeatedPointer   = "/steps/1/after/1";
    constexpr std::string_view gracePointer      = "/pacing/grace_ms";
    constexpr std::string_view extraGracePointer = "/steps/1/extra_grace_ms";
    constexpr std::string_view badStepLabel      = "step 'bad'";
    constexpr std::string_view recoverLabel      = "recover";

    constexpr std::string_view builtSequenceName = "built-by-hand";
    constexpr std::string_view thirdLabel        = "third";

    constexpr std::string_view sequenceFileName  = "grab-interpreter-unit07.json";
    constexpr std::string_view badFileName       = "grab-interpreter-unit07-bad.json";
    constexpr std::string_view missingFileName   = "grab-interpreter-unit07-absent.json";

    // ── Documents ────────────────────────────────────────────

    // The spec's own example, verbatim from the design's section 3.
    constexpr std::string_view specExample           = R"({
  "schema_version": 1,
  "sequence": "login-flow",
  "pacing": { "mode": "grace", "grace_ms": 80 },
  "steps": [
    { "id": "move",  "op": "input.move",     "to": [640, 400] },
    { "id": "wait",  "op": "time.wait",      "ms": 250 },
    { "id": "click", "op": "input.click",    "button": "left" },
    { "id": "shot",  "op": "screen.capture", "out": "a.png", "after": ["click"] },
    { "id": "type",  "op": "input.type",     "text": "hi",   "after": ["click"],
      "extra_grace_ms": 400 }
  ]
})";

    constexpr std::string_view threeImplicitSteps    = R"({
  "steps": [
    { "op": "input.warp", "to": [10, 20] },
    { "op": "time.wait",  "ms": 250 },
    { "op": "input.click" }
  ]
})";

    constexpr std::string_view explicitAfterDocument = R"({
  "steps": [
    { "id": "root", "op": "input.click" },
    { "op": "time.wait", "ms": 250 },
    { "op": "input.type", "text": "hi", "after": ["root"] }
  ]
})";

    constexpr std::string_view forkDocument          = R"({
  "steps": [
    { "id": "click", "op": "input.click" },
    { "op": "screen.capture", "out": "a.png", "after": ["click"] },
    { "op": "input.type", "text": "hi", "after": ["click"] }
  ]
})";

    constexpr std::string_view unknownOpDocument     = R"({
  "steps": [
    { "op": "input.click" },
    { "op": "time.wait", "ms": 250 },
    { "op": "input.click" },
    { "id": "bad", "op": "input.frobnicate" }
  ]
})";

    // Every one of these resolves through command_kind() and has no payload
    // struct, so it is a different author mistake from a misspelling.
    constexpr std::string_view doctorDocument          = R"({
  "steps": [
    { "op": "input.click" },
    { "op": "time.wait", "ms": 250 },
    { "op": "input.click" },
    { "id": "bad", "op": "system.doctor" }
  ]
})";

    constexpr std::string_view dragCurveDocument       = R"({
  "steps": [
    { "op": "input.click" },
    { "op": "time.wait", "ms": 250 },
    { "op": "input.click" },
    { "id": "bad", "op": "input.drag_curve" }
  ]
})";

    constexpr std::string_view overlayTrailDocument    = R"({
  "steps": [
    { "op": "input.click" },
    { "op": "time.wait", "ms": 250 },
    { "op": "input.click" },
    { "id": "bad", "op": "overlay.trail" }
  ]
})";

    constexpr std::string_view sessionDocument         = R"({
  "steps": [
    { "op": "input.click" },
    { "op": "time.wait", "ms": 250 },
    { "op": "input.click" },
    { "id": "bad", "op": "session.open" }
  ]
})";

    constexpr std::string_view playDocument            = R"({
  "steps": [
    { "op": "input.click" },
    { "op": "time.wait", "ms": 250 },
    { "op": "input.click" },
    { "id": "bad", "op": "system.play" }
  ]
})";

    constexpr std::string_view danglingAfterDocument   = R"({
  "steps": [
    { "id": "here", "op": "input.click" },
    { "id": "there", "op": "input.click", "after": ["nowhere"] }
  ]
})";

    constexpr std::string_view duplicateLabelDocument  = R"({
  "steps": [
    { "id": "same", "op": "input.click" },
    { "id": "same", "op": "input.click" }
  ]
})";

    constexpr std::string_view cycleDocument           = R"({
  "steps": [
    { "id": "a", "op": "input.click", "after": ["c"] },
    { "id": "b", "op": "input.click", "after": ["a"] },
    { "id": "c", "op": "input.click", "after": ["b"] }
  ]
})";

    constexpr std::string_view selfEdgeDocument        = R"({
  "steps": [
    { "id": "loop", "op": "input.click", "after": ["loop"] }
  ]
})";

    constexpr std::string_view repeatedAfterDocument   = R"({
  "steps": [
    { "id": "once", "op": "input.click" },
    { "id": "twice", "op": "input.click", "after": ["once", "once"] }
  ]
})";

    constexpr std::string_view identicalClicksDocument = R"({
  "steps": [
    { "op": "input.click", "button": "left" },
    { "op": "input.click", "button": "left" }
  ]
})";

    constexpr std::string_view strictWithExtraGrace    = R"({
  "pacing": { "mode": "strict" },
  "steps": [
    { "id": "a", "op": "input.click" },
    { "id": "b", "op": "input.type", "text": "hi", "extra_grace_ms": 400 }
  ]
})";

    constexpr std::string_view preciseWithExtraGrace   = R"({
  "pacing": { "mode": "precise", "grace_ms": 80 },
  "steps": [
    { "id": "a", "op": "input.click" },
    { "id": "b", "op": "input.type", "text": "hi", "extra_grace_ms": 400 }
  ]
})";

    constexpr std::string_view emptyStepsDocument      = R"({ "steps": [] })";

    constexpr std::string_view missingStepsDocument    = R"({ "sequence": "nothing" })";

    constexpr std::string_view unknownModeDocument     = R"({
  "pacing": { "mode": "eventually" },
  "steps": [ { "op": "input.click" } ]
})";

    constexpr std::string_view negativeGraceDocument   = R"({
  "pacing": { "mode": "grace", "grace_ms": -1 },
  "steps": [ { "op": "input.click" } ]
})";

    constexpr std::string_view negativeExtraGraceDocument = R"({
  "steps": [
    { "op": "input.click" },
    { "op": "input.click", "extra_grace_ms": -1 }
  ]
})";

    constexpr std::string_view waitWithoutDuration        = R"({
  "steps": [ { "id": "w", "op": "time.wait" } ]
})";

    constexpr std::string_view captureWithBothTargets     = R"({
  "steps": [
    { "id": "shot", "op": "screen.capture", "out": "a.png", "locator": "window:1" }
  ]
})";

    constexpr std::string_view explicitRootDocument       = R"({
  "steps": [
    { "id": "a", "op": "input.click" },
    { "id": "b", "op": "input.click", "after": [] }
  ]
})";

    constexpr std::string_view captureByLocatorDocument   = R"({
  "steps": [
    { "id": "shot", "op": "screen.capture", "locator": "window:title=Firefox" }
  ]
})";

    constexpr std::string_view gotoDocument               = R"({
  "steps": [
    { "id": "recover", "op": "input.click" },
    { "id": "risky", "op": "screen.capture", "out": "a.png",
      "on_error": "goto:recover" }
  ]
})";

    constexpr std::string_view gotoNowhereDocument        = R"({
  "steps": [
    { "id": "recover", "op": "input.click" },
    { "id": "risky", "op": "screen.capture", "out": "a.png",
      "on_error": "goto:elsewhere" }
  ]
})";

    constexpr std::string_view buttonSpellingsDocument    = R"({
  "steps": [
    { "op": "input.click", "button": "middle" },
    { "op": "input.press", "button": 3 },
    { "op": "input.release", "button": "wheel_down" }
  ]
})";

    // One step per loadable command: 23 of the descriptor table's 38, which is
    // every alternative of the Command variant. The eight overlay steps are
    // the tail of it.
    constexpr std::string_view everyOpDocument = R"({
  "schema_version": 1,
  "sequence": "every-op",
  "pacing": { "mode": "precise", "grace_ms": 25 },
  "steps": [
    { "id": "warp",    "op": "input.warp",     "to": [10, 20] },
    { "id": "move",    "op": "input.move",     "from": [10, 20], "to": [30, 40],
      "options": { "steps": 8, "step_dwell_ms": 4, "path": "cubic" } },
    { "id": "follow",  "op": "input.follow",
      "curve": [[0.0, 0.0], [10.5, 20.25], [30.0, 40.0]] },
    { "id": "press",   "op": "input.press",    "button": "middle" },
    { "id": "release", "op": "input.release",  "button": "middle" },
    { "id": "click",   "op": "input.click",    "button": "left" },
    { "id": "clickat", "op": "input.click_at", "at": [50, 60], "button": "right" },
    { "id": "drag",    "op": "input.drag",     "from": [1, 2], "to": [3, 4],
      "button": 1 },
    { "id": "scroll",  "op": "input.scroll",   "dx": -1, "dy": 3 },
    { "id": "keydown", "op": "input.key_down", "key": "Control_L" },
    { "id": "key",     "op": "input.key",      "key": "c" },
    { "id": "keyup",   "op": "input.key_up",   "key": "Control_L" },
    { "id": "type",    "op": "input.type",     "text": "héllo ⌘",
      "extra_grace_ms": 400 },
    { "id": "wait",    "op": "time.wait",      "ms": 250, "on_error": "continue" },
    { "id": "shot",    "op": "screen.capture", "out": "a.png",
      "after": ["click", "type"], "on_error": "goto:warp" },
    { "id": "oadd",     "op": "overlay.add",    "handle": "c01",
      "shape": { "ellipse": { "center": [300, 200], "radius": 48 },
                 "stroke": { "color": "#4ecea9", "width": 3 },
                 "fill": { "color": "#4ecea933" }, "z": 10 } },
    { "id": "oupdate",  "op": "overlay.update", "handle": "c01",
      "shape": { "rect": { "x": 100, "y": 100, "w": 90, "h": 90 },
                 "lifetime": { "ttl_ms": 750 }, "band": "trail",
                 "animation": { "opacity": { "easing": "out_cubic",
                                             "duration_ms": 200,
                                             "from": 0.0, "to": 1.0 } } } },
    { "id": "oattach",  "op": "overlay.attach", "handle": "c01",
      "offset": [-4, -6] },
    { "id": "odetach",  "op": "overlay.detach", "handle": "c01" },
    { "id": "ograb",    "op": "overlay.grab" },
    { "id": "orelease", "op": "overlay.release" },
    { "id": "oremove",  "op": "overlay.remove", "handle": "c01" },
    { "id": "oclear",   "op": "overlay.clear" }
  ]
})";

    // ── Overlay values the assertions expect ─────────────────

    constexpr double           rectX               = 100.0;
    constexpr double           rectY               = 100.0;
    constexpr double           rectWidth           = 90.0;
    constexpr double           rectHeight          = 90.0;
    constexpr double           ellipseCenterX      = 300.0;
    constexpr double           ellipseCenterY      = 200.0;
    constexpr double           ellipseRadius       = 48.0;
    constexpr double           ellipseRadiusX      = 60.0;
    constexpr double           ellipseRadiusY      = 40.0;
    constexpr float            strokeWidth         = 3.0F;
    constexpr std::int32_t     shapeZ              = 10;
    constexpr std::int32_t     attachOffsetX       = -4;
    constexpr std::int32_t     attachOffsetY       = -6;

    constexpr std::size_t      polygonPointCount   = 3U;
    constexpr std::size_t      pathCommandCount    = 4U;
    constexpr std::size_t      closedPathCommands  = 2U;
    constexpr std::size_t      bezierControlCount  = 3U;
    constexpr std::size_t      fourSteps           = 4U;

    constexpr std::uint8_t     strokeRed           = 0X4EU;
    constexpr std::uint8_t     strokeGreen         = 0XCEU;
    constexpr std::uint8_t     strokeBlue          = 0XA9U;
    constexpr std::uint8_t     opaqueAlphaValue    = 0XFFU;
    constexpr std::uint8_t     fillAlphaValue      = 0X33U;

    constexpr std::int64_t     ttlMilliseconds     = 750;
    constexpr std::int64_t     fadeMilliseconds    = 250;
    constexpr std::int64_t     channelMilliseconds = 200;
    constexpr double           scaleFrom           = 0.5;
    constexpr double           scaleTo             = 1.0;
    constexpr double           opacityFrom         = 0.0;
    constexpr double           opacityTo           = 1.0;
    constexpr double           translateDx         = 12.0;
    constexpr double           translateDy         = -8.0;
    constexpr double           revealFrom          = 0.0;
    constexpr double           revealTo            = 1.0;

    constexpr std::string_view liveHandle          = "c01";

    // ── Overlay message fragments ────────────────────────────

    constexpr std::string_view geometryCountPhrase = "exactly one geometry key";
    constexpr std::string_view foundNonePhrase     = "found none";
    constexpr std::string_view rectKeyName         = "rect";
    constexpr std::string_view ellipseKeyName      = "ellipse";
    constexpr std::string_view colorFormsPhrase    = "'#rrggbb' or '#rrggbbaa'";
    constexpr std::string_view beforeAddPhrase     = "before any overlay.add creates it";
    constexpr std::string_view stillLivePhrase     = "while it is still live";
    constexpr std::string_view negativePhrase      = "must not be negative";
    constexpr std::string_view shapePointer        = "/steps/0/shape";
    constexpr std::string_view colorPointer        = "/steps/0/shape/stroke/color";
    constexpr std::string_view radiusPointer       = "/steps/0/shape/ellipse/radius";
    constexpr std::string_view handlePointer       = "/steps/0/handle";
    constexpr std::string_view secondHandlePointer = "/steps/1/handle";
    constexpr std::string_view radiusKeyText       = R"("radius")";
    constexpr std::string_view radiusXKeyText      = R"("radius_x")";
    constexpr std::string_view handleKeyText       = R"("handle")";
    constexpr std::string_view closedKeyText       = R"("closed")";

    // ── Overlay documents ────────────────────────────────────

    // One add per Geometry alternative, spelled exactly as design §3.2 does.
    constexpr std::string_view overlayGeometriesDocument = R"({
  "steps": [
    { "id": "r", "op": "overlay.add", "handle": "rect-01",
      "shape": { "rect": { "x": 100, "y": 100, "w": 90, "h": 90 } } },
    { "id": "e", "op": "overlay.add", "handle": "ellipse-01",
      "shape": { "ellipse": { "center": [300, 200], "radius": 48 } } },
    { "id": "o", "op": "overlay.add", "handle": "ellipse-02",
      "shape": { "ellipse": { "center": [300, 200],
                              "radius_x": 60, "radius_y": 40 } } },
    { "id": "p", "op": "overlay.add", "handle": "polygon-01",
      "shape": { "polygon": [[0, 0], [10, 0], [10, 10]] } },
    { "id": "q", "op": "overlay.add", "handle": "path-01",
      "shape": { "path": [ {"move": [0, 0]}, {"line": [10, 10]},
                           {"bezier": [[20, 0], [30, 20], [40, 10]]}, "close" ] } }
  ]
})";

    // The one deviation from §3.2's shape grammar: a path may also be written
    // as an object, which is the only way to spell overlay::Path::closed. The
    // array form of §3.2 cannot carry it, and a to_json that cannot write a
    // value its own type holds is a hole in the round trip.
    constexpr std::string_view closedPathDocument       = R"({
  "steps": [
    { "id": "c", "op": "overlay.add", "handle": "c1",
      "shape": { "path": { "commands": [ {"move": [0, 0]}, {"line": [10, 10]} ],
                           "closed": true } } }
  ]
})";

    constexpr std::string_view ellipseShorthandDocument = R"({
  "steps": [
    { "id": "e", "op": "overlay.add", "handle": "e1",
      "shape": { "ellipse": { "center": [300, 200], "radius": 48 } } }
  ]
})";

    constexpr std::string_view overlayColorsDocument    = R"({
  "steps": [
    { "id": "c", "op": "overlay.add", "handle": "c1",
      "shape": { "rect": { "x": 100, "y": 100, "w": 90, "h": 90 },
                 "stroke": { "color": "#4ecea9", "width": 3 },
                 "fill": { "color": "#4ecea933" }, "z": 10 } }
  ]
})";

    constexpr std::string_view overlayLifetimesDocument = R"({
  "steps": [
    { "id": "p", "op": "overlay.add", "handle": "p1",
      "shape": { "rect": { "x": 100, "y": 100, "w": 90, "h": 90 },
                 "lifetime": "persistent" } },
    { "id": "t", "op": "overlay.add", "handle": "t1",
      "shape": { "rect": { "x": 100, "y": 100, "w": 90, "h": 90 },
                 "lifetime": { "ttl_ms": 750 } } },
    { "id": "f", "op": "overlay.add", "handle": "f1",
      "shape": { "rect": { "x": 100, "y": 100, "w": 90, "h": 90 },
                 "lifetime": { "fade_ms": 250 } } }
  ]
})";

    constexpr std::string_view overlayAnimationDocument = R"({
  "steps": [
    { "id": "s", "op": "overlay.add", "handle": "s1",
      "shape": { "rect": { "x": 100, "y": 100, "w": 90, "h": 90 },
                 "animation": { "scale": { "easing": "in_quad",
                                           "duration_ms": 200,
                                           "from": 0.5, "to": 1.0 } } } },
    { "id": "o", "op": "overlay.add", "handle": "o1",
      "shape": { "rect": { "x": 100, "y": 100, "w": 90, "h": 90 },
                 "animation": { "opacity": { "easing": "out_cubic",
                                             "duration_ms": 200,
                                             "from": 0.0, "to": 1.0 } } } },
    { "id": "t", "op": "overlay.add", "handle": "t1",
      "shape": { "rect": { "x": 100, "y": 100, "w": 90, "h": 90 },
                 "animation": { "translate": { "easing": "linear",
                                               "duration_ms": 200,
                                               "dx": 12.0, "dy": -8.0 } } } },
    { "id": "v", "op": "overlay.add", "handle": "v1",
      "shape": { "rect": { "x": 100, "y": 100, "w": 90, "h": 90 },
                 "animation": { "reveal": { "easing": "in_out_cubic",
                                            "duration_ms": 200,
                                            "axis": "y", "from_edge": "max",
                                            "from": 0.0, "to": 1.0 } } } }
  ]
})";

    // Neither stroke nor fill: legal, invisible, and a useful thing to be able
    // to say. A handle is optional too — this shape is fire-and-forget.
    constexpr std::string_view unstyledShapeDocument   = R"({
  "steps": [
    { "id": "u", "op": "overlay.add",
      "shape": { "rect": { "x": 100, "y": 100, "w": 90, "h": 90 } } }
  ]
})";

    constexpr std::string_view twoGeometryKeysDocument = R"({
  "steps": [
    { "id": "two", "op": "overlay.add", "handle": "t1",
      "shape": { "rect": { "x": 100, "y": 100, "w": 90, "h": 90 },
                 "ellipse": { "center": [300, 200], "radius": 48 } } }
  ]
})";

    constexpr std::string_view noGeometryKeyDocument   = R"({
  "steps": [
    { "id": "none", "op": "overlay.add", "handle": "n1",
      "shape": { "stroke": { "color": "#4ecea9", "width": 3 } } }
  ]
})";

    constexpr std::string_view badColorDocument        = R"({
  "steps": [
    { "id": "bad", "op": "overlay.add", "handle": "b1",
      "shape": { "rect": { "x": 100, "y": 100, "w": 90, "h": 90 },
                 "stroke": { "color": "4ecea9", "width": 3 } } }
  ]
})";

    constexpr std::string_view negativeRadiusDocument  = R"({
  "steps": [
    { "id": "neg", "op": "overlay.add", "handle": "n1",
      "shape": { "ellipse": { "center": [300, 200], "radius": -48 } } }
  ]
})";

    constexpr std::string_view handleBeforeAddDocument = R"({
  "steps": [
    { "id": "early", "op": "overlay.update", "handle": "c01",
      "shape": { "rect": { "x": 100, "y": 100, "w": 90, "h": 90 } } },
    { "id": "late", "op": "overlay.add", "handle": "c01",
      "shape": { "rect": { "x": 100, "y": 100, "w": 90, "h": 90 } } }
  ]
})";

    constexpr std::string_view duplicateHandleDocument = R"({
  "steps": [
    { "id": "first", "op": "overlay.add", "handle": "c01",
      "shape": { "rect": { "x": 100, "y": 100, "w": 90, "h": 90 } } },
    { "id": "second", "op": "overlay.add", "handle": "c01",
      "shape": { "rect": { "x": 100, "y": 100, "w": 90, "h": 90 } } }
  ]
})";

    // A remove retires the name, so the third step is a NEW shape wearing an
    // old label rather than a collision.
    constexpr std::string_view reusedHandleDocument = R"({
  "steps": [
    { "id": "first", "op": "overlay.add", "handle": "c01",
      "shape": { "rect": { "x": 100, "y": 100, "w": 90, "h": 90 } } },
    { "id": "gone", "op": "overlay.remove", "handle": "c01" },
    { "id": "again", "op": "overlay.add", "handle": "c01",
      "shape": { "rect": { "x": 100, "y": 100, "w": 90, "h": 90 } } }
  ]
})";

    // ── Helpers ──────────────────────────────────────────────

    [[nodiscard]]
    const grab::overlay::Shape&
    added_shape( const grab::sequence::Step& step )
    {
        return std::get<grab::sequence::OverlayAddCommand>( step.command ).shape;
    }

    [[nodiscard]]
    constexpr grab::sequence::StepId
    step_id( Half index ) noexcept
    {
        return grab::sequence::StepId{ index, grab::sequence::StepId::firstGeneration };
    }

    [[nodiscard]]
    bool
    mentions( const std::string& message,
              std::string_view   fragment )
    {
        return message.find( fragment ) != std::string::npos;
    }

    [[nodiscard]]
    std::string
    rejection_of( std::string_view document )
    {
        const auto parsed = parse( document );
        if( parsed.has_value() )
        {
            return {};
        }
        return parsed.error().message;
    }

    // Identity in the sense the round-trip claims: same ids, same edges, same
    // labels, same policies, same order, and a byte-identical serialization.
    void
    expect_identical( const Sequence& lhs,
                      const Sequence& rhs )
    {
        EXPECT_EQ( lhs.name(), rhs.name() );
        EXPECT_EQ( lhs.pacing().mode, rhs.pacing().mode );
        EXPECT_EQ( lhs.pacing().grace, rhs.pacing().grace );

        ASSERT_EQ( lhs.steps().size(), rhs.steps().size() );
        for( std::size_t index = 0U; index < lhs.steps().size(); ++index )
        {
            const auto& leftStep  = lhs.steps()[index];
            const auto& rightStep = rhs.steps()[index];

            EXPECT_EQ( leftStep.id.bits(), rightStep.id.bits() );
            EXPECT_EQ( leftStep.label, rightStep.label );
            EXPECT_EQ( leftStep.on_error, rightStep.on_error );
            EXPECT_EQ( leftStep.on_error_target, rightStep.on_error_target );
            EXPECT_EQ( leftStep.extra_grace, rightStep.extra_grace );
            EXPECT_EQ( grab::sequence::kind_of( leftStep.command ),
                       grab::sequence::kind_of( rightStep.command ) );

            ASSERT_EQ( leftStep.after.size(), rightStep.after.size() );
            for( std::size_t slot = 0U; slot < leftStep.after.size(); ++slot )
            {
                EXPECT_EQ( leftStep.after[slot].bits(), rightStep.after[slot].bits() );
            }
        }

        ASSERT_EQ( lhs.order().size(), rhs.order().size() );
        for( std::size_t index = 0U; index < lhs.order().size(); ++index )
        {
            EXPECT_EQ( lhs.order()[index].bits(), rhs.order()[index].bits() );
        }

        const auto leftText  = to_json( lhs );
        const auto rightText = to_json( rhs );
        ASSERT_TRUE( leftText.has_value() ) << leftText.error().message;
        ASSERT_TRUE( rightText.has_value() ) << rightText.error().message;
        EXPECT_EQ( *leftText, *rightText );
    }

    void
    expect_round_trips( std::string_view document )
    {
        const auto first = parse( document );
        ASSERT_TRUE( first.has_value() ) << first.error().message;
        const auto text = to_json( *first );
        ASSERT_TRUE( text.has_value() ) << text.error().message;
        const auto second = parse( *text );
        ASSERT_TRUE( second.has_value() ) << second.error().message;
        expect_identical( *first, *second );
    }

    [[nodiscard]]
    std::filesystem::path
    scratch_file( std::string_view name )
    {
        return std::filesystem::path{ ::testing::TempDir() } / name;
    }

    // ── Implicit and explicit dependencies ───────────────────

    TEST( Interpreter,
          AStepWithoutAfterDependsOnThePrecedingStep )
    {
        const auto parsed = parse( threeImplicitSteps );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto steps = parsed->steps();
        ASSERT_EQ( steps.size(), threeSteps );

        // The first step depends on nothing; a plain list then reads
        // top-to-bottom without an `after` on every line.
        EXPECT_TRUE( steps[firstIndex].after.empty() );

        ASSERT_EQ( steps[secondIndex].after.size(), oneEdge );
        EXPECT_EQ( steps[secondIndex].after.front(), step_id( firstIndex ) );

        ASSERT_EQ( steps[thirdIndex].after.size(), oneEdge );
        EXPECT_EQ( steps[thirdIndex].after.front(), step_id( secondIndex ) );
    }

    TEST( Interpreter,
          ExplicitAfterOverridesTheImplicitEdge )
    {
        const auto parsed = parse( explicitAfterDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto steps = parsed->steps();
        ASSERT_EQ( steps.size(), threeSteps );
        ASSERT_EQ( steps[thirdIndex].after.size(), oneEdge );
        EXPECT_EQ( steps[thirdIndex].after.front(), step_id( firstIndex ) );
    }

    TEST( Interpreter,
          TwoStepsNamingOnePredecessorFork )
    {
        const auto parsed = parse( forkDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto steps = parsed->steps();
        ASSERT_EQ( steps.size(), threeSteps );
        ASSERT_EQ( steps[secondIndex].after.size(), oneEdge );
        ASSERT_EQ( steps[thirdIndex].after.size(), oneEdge );
        EXPECT_EQ( steps[secondIndex].after.front(), step_id( firstIndex ) );
        EXPECT_EQ( steps[thirdIndex].after.front(), step_id( firstIndex ) );
    }

    TEST( Interpreter,
          AnExplicitlyEmptyAfterDeclaresARootAndSurvivesTheRoundTrip )
    {
        const auto parsed = parse( explicitRootDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;
        ASSERT_EQ( parsed->steps().size(), twoSteps );
        EXPECT_TRUE( parsed->steps()[secondIndex].after.empty() );

        expect_round_trips( explicitRootDocument );
    }

    // ── Op resolution ────────────────────────────────────────

    TEST( Interpreter,
          AnUnknownOpNamesTheOpTheStepAndAJsonPointer )
    {
        const auto parsed = parse( unknownOpDocument );
        ASSERT_FALSE( parsed.has_value() );

        const auto& message = parsed.error().message;
        EXPECT_TRUE( mentions( message, unknownOpPhrase ) ) << message;
        EXPECT_TRUE( mentions( message, unknownOpName ) ) << message;
        EXPECT_TRUE( mentions( message, opPointer ) ) << message;
        EXPECT_TRUE( mentions( message, badStepLabel ) ) << message;
    }

    TEST( Interpreter,
          ATableKnownOpWithNoPayloadGetsADifferentMessageFromAnUnknownOp )
    {
        constexpr auto    documents = std::to_array<std::string_view>( {
            doctorDocument,
            dragCurveDocument,
            overlayTrailDocument,
            sessionDocument,
            playDocument,
        } );

        const std::string unknown   = rejection_of( unknownOpDocument );
        ASSERT_FALSE( unknown.empty() );

        for( const auto document : documents )
        {
            const std::string message = rejection_of( document );
            ASSERT_FALSE( message.empty() ) << document;

            // The name is real; the verb simply cannot be a step. That is a
            // different author mistake from a misspelling and must not share
            // its message.
            EXPECT_TRUE( mentions( message, unavailablePhrase ) ) << message;
            EXPECT_FALSE( mentions( message, unknownOpPhrase ) ) << message;
            EXPECT_TRUE( mentions( message, opPointer ) ) << message;
            EXPECT_TRUE( mentions( message, badStepLabel ) ) << message;
            EXPECT_NE( message, unknown );
        }
    }

    TEST( Interpreter,
          EverySequenceCapableOpParses )
    {
        const auto parsed = parse( everyOpDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;
        ASSERT_EQ( parsed->steps().size(), everyOpStepCount );

        std::vector<grab::CommandKind> kinds;
        kinds.reserve( parsed->steps().size() );
        for( const auto& step : parsed->steps() )
        {
            kinds.push_back( grab::sequence::kind_of( step.command ) );
        }
        std::ranges::sort( kinds );
        EXPECT_EQ( std::ranges::unique( kinds ).begin(), kinds.end() );
    }

    // ── Graph faults ─────────────────────────────────────────

    TEST( Interpreter,
          ADanglingAfterIsRejected )
    {
        const std::string message = rejection_of( danglingAfterDocument );
        ASSERT_FALSE( message.empty() );
        EXPECT_TRUE( mentions( message, danglingPhrase ) ) << message;
        EXPECT_TRUE( mentions( message, danglingPointer ) ) << message;
    }

    TEST( Interpreter,
          ADuplicateLabelIsRejected )
    {
        const std::string message = rejection_of( duplicateLabelDocument );
        ASSERT_FALSE( message.empty() );
        EXPECT_TRUE( mentions( message, duplicatePhrase ) ) << message;
        EXPECT_TRUE( mentions( message, duplicatePointer ) ) << message;
    }

    TEST( Interpreter,
          ACycleIsRejected )
    {
        const std::string message = rejection_of( cycleDocument );
        ASSERT_FALSE( message.empty() );
        EXPECT_TRUE( mentions( message, cyclePhrase ) ) << message;
    }

    TEST( Interpreter,
          ASelfEdgeIsRejected )
    {
        // add_edge drops a self-loop and returns false, so the topological
        // sort never sees one: rejecting it is the loader's job.
        const std::string message = rejection_of( selfEdgeDocument );
        ASSERT_FALSE( message.empty() );
        EXPECT_TRUE( mentions( message, selfEdgePhrase ) ) << message;
        EXPECT_TRUE( mentions( message, selfEdgePointer ) ) << message;
    }

    TEST( Interpreter,
          ARepeatedAfterEntryIsRejected )
    {
        const std::string message = rejection_of( repeatedAfterDocument );
        ASSERT_FALSE( message.empty() );
        EXPECT_TRUE( mentions( message, repeatedPhrase ) ) << message;
        EXPECT_TRUE( mentions( message, repeatedPointer ) ) << message;
    }

    TEST( Interpreter,
          EveryGraphFaultGetsItsOwnMessage )
    {
        const std::array<std::string, graphFaultCount> messages{
            rejection_of( danglingAfterDocument ),
            rejection_of( duplicateLabelDocument ),
            rejection_of( cycleDocument ),
            rejection_of( selfEdgeDocument ),
            rejection_of( repeatedAfterDocument ),
        };

        for( std::size_t left = 0U; left < messages.size(); ++left )
        {
            ASSERT_FALSE( messages[left].empty() ) << left;
            for( std::size_t right = left + 1U; right < messages.size(); ++right )
            {
                EXPECT_NE( messages[left], messages[right] );
            }
        }
    }

    // ── Identity ─────────────────────────────────────────────

    TEST( Interpreter,
          ByteIdenticalUnlabelledClicksGetDifferentStepIds )
    {
        const auto parsed = parse( identicalClicksDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto steps = parsed->steps();
        ASSERT_EQ( steps.size(), twoSteps );
        EXPECT_TRUE( steps[firstIndex].label.empty() );
        EXPECT_TRUE( steps[secondIndex].label.empty() );

        // Positional, not content-derived: a hash of the payload would collide
        // exactly here, which is the case the format needs distinct.
        EXPECT_NE( steps[firstIndex].id.bits(), steps[secondIndex].id.bits() );
        EXPECT_EQ( steps[firstIndex].id, step_id( firstIndex ) );
        EXPECT_EQ( steps[secondIndex].id, step_id( secondIndex ) );
    }

    // ── Pacing ───────────────────────────────────────────────

    TEST( Interpreter,
          ExtraGraceLoadsUnderStrictAndTheModeStaysStrict )
    {
        const auto parsed = parse( strictWithExtraGrace );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        EXPECT_EQ( parsed->pacing().mode, grab::sequence::PacingMode::Strict );
        ASSERT_EQ( parsed->steps().size(), twoSteps );
        EXPECT_EQ( parsed->steps()[secondIndex].extra_grace,
                   std::chrono::milliseconds{ extraGraceMilliseconds } );
    }

    TEST( Interpreter,
          TheSameDocumentUnderTwoModesYieldsTheSameStepIds )
    {
        const auto strict  = parse( strictWithExtraGrace );
        const auto precise = parse( preciseWithExtraGrace );
        ASSERT_TRUE( strict.has_value() ) << strict.error().message;
        ASSERT_TRUE( precise.has_value() ) << precise.error().message;

        ASSERT_EQ( strict->steps().size(), precise->steps().size() );
        for( std::size_t index = 0U; index < strict->steps().size(); ++index )
        {
            EXPECT_EQ( strict->steps()[index].id.bits(),
                       precise->steps()[index].id.bits() );
        }
        EXPECT_NE( strict->pacing().mode, precise->pacing().mode );
    }

    TEST( Interpreter,
          AnUnknownPacingModeIsRejected )
    {
        const std::string message = rejection_of( unknownModeDocument );
        ASSERT_FALSE( message.empty() );
        EXPECT_TRUE( mentions( message, unknownModePhrase ) ) << message;
    }

    TEST( Interpreter,
          ANegativeGraceIsRejected )
    {
        const std::string document = rejection_of( negativeGraceDocument );
        ASSERT_FALSE( document.empty() );
        EXPECT_TRUE( mentions( document, gracePointer ) ) << document;

        const std::string step = rejection_of( negativeExtraGraceDocument );
        ASSERT_FALSE( step.empty() );
        EXPECT_TRUE( mentions( step, extraGracePointer ) ) << step;
    }

    // ── Payloads ─────────────────────────────────────────────

    TEST( Interpreter,
          WaitRequiresADeclaredDuration )
    {
        // time.wait is the one op whose duration is mandatory in JSON; every
        // other Timed op takes its dwell from defaulted options.
        const std::string message = rejection_of( waitWithoutDuration );
        ASSERT_FALSE( message.empty() );
    }

    TEST( Interpreter,
          CaptureNeedsExactlyOneTarget )
    {
        const std::string message = rejection_of( captureWithBothTargets );
        ASSERT_FALSE( message.empty() );
    }

    TEST( Interpreter,
          ButtonNamesAndCodesBothLoad )
    {
        const auto parsed = parse( buttonSpellingsDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto steps = parsed->steps();
        ASSERT_EQ( steps.size(), threeSteps );
        EXPECT_EQ(
            std::get<grab::sequence::ClickCommand>( steps[firstIndex].command ).button,
            middleButton
        );
        EXPECT_EQ(
            std::get<grab::sequence::PressCommand>( steps[secondIndex].command ).button,
            rightButton
        );
        EXPECT_EQ(
            std::get<grab::sequence::ReleaseCommand>( steps[thirdIndex].command ).button,
            wheelDownButton
        );

        expect_round_trips( buttonSpellingsDocument );
    }

    TEST( Interpreter,
          PayloadValuesSurviveTheParse )
    {
        const auto parsed = parse( threeImplicitSteps );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto steps = parsed->steps();
        ASSERT_EQ( steps.size(), threeSteps );

        const auto& warp =
            std::get<grab::sequence::WarpCommand>( steps[firstIndex].command );
        EXPECT_EQ( warp.to.x, warpX );
        EXPECT_EQ( warp.to.y, warpY );

        const auto& wait =
            std::get<grab::sequence::WaitCommand>( steps[secondIndex].command );
        EXPECT_EQ(
            wait.duration,
            std::chrono::nanoseconds{ std::chrono::milliseconds{ waitMilliseconds } }
        );

        EXPECT_EQ(
            std::get<grab::sequence::ClickCommand>( steps[thirdIndex].command ).button,
            leftButton
        );
    }

    // ── on_error ─────────────────────────────────────────────

    TEST( Interpreter,
          GotoResolvesItsTargetLabel )
    {
        const auto parsed = parse( gotoDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        ASSERT_EQ( parsed->steps().size(), twoSteps );
        EXPECT_EQ( parsed->steps()[secondIndex].on_error,
                   grab::sequence::ErrorPolicy::Goto );
        EXPECT_EQ( parsed->steps()[secondIndex].on_error_target, recoverLabel );

        expect_round_trips( gotoDocument );
    }

    TEST( Interpreter,
          GotoAtAnUnknownLabelIsRejected )
    {
        const std::string message = rejection_of( gotoNowhereDocument );
        ASSERT_FALSE( message.empty() );
        EXPECT_TRUE( mentions( message, danglingPhrase ) ) << message;
    }

    // ── Document shape ───────────────────────────────────────

    TEST( Interpreter,
          AnEmptyStepListLoadsAsAnEmptySequence )
    {
        const auto parsed = parse( emptyStepsDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;
        EXPECT_EQ( parsed->steps().size(), noSteps );
        EXPECT_EQ( parsed->order().size(), noSteps );
    }

    TEST( Interpreter,
          TheStepsFieldIsRequired )
    {
        const std::string message = rejection_of( missingStepsDocument );
        ASSERT_FALSE( message.empty() );
    }

    // ── Round trip ───────────────────────────────────────────

    TEST( Interpreter,
          ParseToJsonParseIsIdenticalIdsIncluded )
    {
        expect_round_trips( everyOpDocument );
    }

    TEST( Interpreter,
          TheSpecExampleParsesAndRoundTrips )
    {
        const auto parsed = parse( specExample );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto steps = parsed->steps();
        ASSERT_EQ( steps.size(), fiveSteps );
        EXPECT_EQ( parsed->pacing().mode, grab::sequence::PacingMode::Grace );
        EXPECT_EQ( parsed->pacing().grace,
                   std::chrono::milliseconds{ documentGraceMilliseconds } );

        // move -> wait -> click is implicit; shot and type both name click and
        // therefore fork.
        EXPECT_TRUE( steps[firstIndex].after.empty() );
        ASSERT_EQ( steps[secondIndex].after.size(), oneEdge );
        EXPECT_EQ( steps[secondIndex].after.front(), step_id( firstIndex ) );
        ASSERT_EQ( steps[fourthIndex].after.size(), oneEdge );
        EXPECT_EQ( steps[fourthIndex].after.front(), step_id( thirdIndex ) );
        ASSERT_EQ( steps[fifthIndex].after.size(), oneEdge );
        EXPECT_EQ( steps[fifthIndex].after.front(), step_id( thirdIndex ) );

        EXPECT_EQ( steps[fifthIndex].extra_grace,
                   std::chrono::milliseconds{ extraGraceMilliseconds } );

        expect_round_trips( specExample );
    }

    TEST( Interpreter,
          CaptureByLocatorRoundTrips )
    {
        expect_round_trips( captureByLocatorDocument );
    }

    // A Sequence does not have to come from parse(): splice() produces graphs
    // whose predecessors carry no label, and `after` is not expressible as a
    // label there. to_json falls back to the document index, and parse accepts
    // it, so serialization stays total rather than only covering what the
    // grammar happens to have written.
    TEST( Interpreter,
          AnUnlabelledPredecessorSerializesAsADocumentIndex )
    {
        const auto click =
            []( std::string label, std::vector<grab::sequence::StepId> after )
        {
            return grab::sequence::Step{
                .id              = grab::sequence::StepId{},
                .label           = std::move( label ),
                .command         = grab::sequence::Command{ grab::sequence::ClickCommand{
                    .button = leftButton,
                } },
                .after           = std::move( after ),
                .on_error        = grab::sequence::ErrorPolicy::Abort,
                .on_error_target = {},
                .extra_grace     = std::chrono::milliseconds::zero(),
            };
        };

        std::vector<grab::sequence::Step> steps;
        steps.push_back( click( {}, {} ) );
        steps.push_back( click( {}, { step_id( firstIndex ) } ) );
        // Not the preceding step, so `after` must be written out — and step 0
        // has no label to write.
        steps.push_back( click( std::string{ thirdLabel }, { step_id( firstIndex ) } ) );

        auto built = Sequence::build( std::move( steps ),
                                      grab::sequence::PacingOptions{
                                          .mode  = grab::sequence::PacingMode::Strict,
                                          .grace = std::chrono::milliseconds::zero(),
                                      },
                                      std::string{ builtSequenceName } );
        ASSERT_TRUE( built.has_value() ) << built.error().message;

        const auto text = to_json( *built );
        ASSERT_TRUE( text.has_value() ) << text.error().message;
        const auto reparsed = parse( *text );
        ASSERT_TRUE( reparsed.has_value() ) << reparsed.error().message;
        expect_identical( *built, *reparsed );
    }

    TEST( Interpreter,
          AMultiEntryAfterSurvivesTheRoundTrip )
    {
        const auto parsed = parse( everyOpDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto steps = parsed->steps();
        ASSERT_EQ( steps.size(), everyOpStepCount );
        EXPECT_EQ( steps[fifteenthIndex].after.size(), twoEdges );
    }

    // ── Overlay geometry ─────────────────────────────────────

    TEST( Interpreter,
          EveryGeometryAlternativeParsesAndRoundTrips )
    {
        const auto parsed = parse( overlayGeometriesDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto steps = parsed->steps();
        ASSERT_EQ( steps.size(), fiveSteps );

        const auto& rect =
            std::get<grab::overlay::Rect>( added_shape( steps[firstIndex] ).geometry );
        EXPECT_EQ( rect.bounds.x, rectX );
        EXPECT_EQ( rect.bounds.y, rectY );
        EXPECT_EQ( rect.bounds.w, rectWidth );
        EXPECT_EQ( rect.bounds.h, rectHeight );

        const auto& circle = std::get<grab::overlay::Ellipse>(
            added_shape( steps[secondIndex] ).geometry
        );
        EXPECT_EQ( circle.center.x, ellipseCenterX );
        EXPECT_EQ( circle.center.y, ellipseCenterY );

        const auto& oval = std::get<grab::overlay::Ellipse>(
            added_shape( steps[thirdIndex] ).geometry
        );
        EXPECT_EQ( oval.radius_x, ellipseRadiusX );
        EXPECT_EQ( oval.radius_y, ellipseRadiusY );

        const auto& polygon = std::get<grab::overlay::Polygon>(
            added_shape( steps[fourthIndex] ).geometry
        );
        ASSERT_EQ( polygon.points.size(), polygonPointCount );

        const auto& path =
            std::get<grab::overlay::Path>( added_shape( steps[fifthIndex] ).geometry );
        ASSERT_EQ( path.commands.size(), pathCommandCount );
        EXPECT_TRUE(
            std::holds_alternative<grab::overlay::MoveTo>( path.commands[firstIndex] )
        );
        EXPECT_TRUE(
            std::holds_alternative<grab::overlay::LineTo>( path.commands[secondIndex] )
        );
        const auto& bezier =
            std::get<grab::overlay::BezierTo>( path.commands[thirdIndex] );
        EXPECT_EQ( bezier.control.size(), bezierControlCount );

        // The bare string form of ClosePath, the only path command with no
        // operand.
        EXPECT_TRUE( std::holds_alternative<grab::overlay::ClosePath>(
            path.commands[fourthIndex]
        ) );
        // A ClosePath COMMAND closes the active contour; Path::closed closes
        // the last one. The array form spells the first and not the second.
        EXPECT_FALSE( path.closed );

        expect_round_trips( overlayGeometriesDocument );
    }

    TEST( Interpreter,
          TheRadiusShorthandExpandsToEqualRadiiAndIsWrittenBackAsShorthand )
    {
        const auto parsed = parse( ellipseShorthandDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto& circle = std::get<grab::overlay::Ellipse>(
            added_shape( parsed->steps()[firstIndex] ).geometry
        );
        EXPECT_EQ( circle.radius_x, ellipseRadius );
        EXPECT_EQ( circle.radius_y, ellipseRadius );
        EXPECT_EQ( circle.radius_x, circle.radius_y );

        // Equal radii write the shorthand back out, so the spelling a document
        // used is the spelling it keeps.
        const auto text = to_json( *parsed );
        ASSERT_TRUE( text.has_value() ) << text.error().message;
        EXPECT_TRUE( mentions( *text, radiusKeyText ) ) << *text;
        EXPECT_FALSE( mentions( *text, radiusXKeyText ) ) << *text;

        expect_round_trips( ellipseShorthandDocument );
    }

    TEST( Interpreter,
          AClosedPathTakesTheObjectFormAndRoundTrips )
    {
        const auto parsed = parse( closedPathDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto& path = std::get<grab::overlay::Path>(
            added_shape( parsed->steps()[firstIndex] ).geometry
        );
        ASSERT_EQ( path.commands.size(), closedPathCommands );
        EXPECT_TRUE( path.closed );

        // Closed paths write the object form; open ones keep §3.2's bare
        // array, which is what every hand-written document uses.
        expect_round_trips( closedPathDocument );

        const auto text = to_json( *parsed );
        ASSERT_TRUE( text.has_value() ) << text.error().message;
        EXPECT_TRUE( mentions( *text, closedKeyText ) ) << *text;
    }

    TEST( Interpreter,
          TwoGeometryKeysAreRejectedNamingBoth )
    {
        const std::string message = rejection_of( twoGeometryKeysDocument );
        ASSERT_FALSE( message.empty() );
        EXPECT_TRUE( mentions( message, geometryCountPhrase ) ) << message;
        EXPECT_TRUE( mentions( message, rectKeyName ) ) << message;
        EXPECT_TRUE( mentions( message, ellipseKeyName ) ) << message;
        EXPECT_TRUE( mentions( message, shapePointer ) ) << message;
    }

    TEST( Interpreter,
          AShapeWithNoGeometryKeyIsRejected )
    {
        const std::string message = rejection_of( noGeometryKeyDocument );
        ASSERT_FALSE( message.empty() );
        EXPECT_TRUE( mentions( message, geometryCountPhrase ) ) << message;
        EXPECT_TRUE( mentions( message, foundNonePhrase ) ) << message;
        EXPECT_TRUE( mentions( message, shapePointer ) ) << message;
    }

    TEST( Interpreter,
          ANegativeRadiusIsRejectedAtItsOwnPointer )
    {
        const std::string message = rejection_of( negativeRadiusDocument );
        ASSERT_FALSE( message.empty() );
        EXPECT_TRUE( mentions( message, negativePhrase ) ) << message;
        EXPECT_TRUE( mentions( message, radiusPointer ) ) << message;
    }

    // ── Overlay styling ──────────────────────────────────────

    TEST( Interpreter,
          BothColorFormsLoadAndTheShortOneSurvivesTheRoundTrip )
    {
        const auto parsed = parse( overlayColorsDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto& shape = added_shape( parsed->steps()[firstIndex] );
        ASSERT_TRUE( shape.stroke.has_value() );
        ASSERT_TRUE( shape.fill.has_value() );

        EXPECT_EQ( shape.stroke->color.r, strokeRed );
        EXPECT_EQ( shape.stroke->color.g, strokeGreen );
        EXPECT_EQ( shape.stroke->color.b, strokeBlue );
        // Three channels mean opaque, which is what #rrggbb means everywhere
        // else in grab.
        EXPECT_EQ( shape.stroke->color.a, opaqueAlphaValue );
        EXPECT_EQ( shape.stroke->width_px, strokeWidth );

        EXPECT_EQ( shape.fill->color.r, strokeRed );
        EXPECT_EQ( shape.fill->color.a, fillAlphaValue );
        EXPECT_EQ( shape.z, shapeZ );

        expect_round_trips( overlayColorsDocument );
    }

    TEST( Interpreter,
          AColorThatIsNeitherFormIsRejectedNamingBothForms )
    {
        const std::string message = rejection_of( badColorDocument );
        ASSERT_FALSE( message.empty() );
        EXPECT_TRUE( mentions( message, colorFormsPhrase ) ) << message;
        EXPECT_TRUE( mentions( message, colorPointer ) ) << message;
    }

    TEST( Interpreter,
          AShapeWithNeitherStrokeNorFillIsLegal )
    {
        // Legal and invisible, which is a useful thing to be able to say — a
        // shape can exist to be attached, measured or updated later.
        const auto parsed = parse( unstyledShapeDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto& shape = added_shape( parsed->steps()[firstIndex] );
        EXPECT_FALSE( shape.stroke.has_value() );
        EXPECT_FALSE( shape.fill.has_value() );
        EXPECT_TRUE(
            std::holds_alternative<grab::overlay::Persistent>( shape.lifetime )
        );
        EXPECT_EQ( shape.band, grab::overlay::Band::Annotation );
        EXPECT_FALSE( shape.animation.has_value() );

        expect_round_trips( unstyledShapeDocument );
    }

    TEST( Interpreter,
          AllThreeLifetimesLoadAndRoundTrip )
    {
        const auto parsed = parse( overlayLifetimesDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto steps = parsed->steps();
        ASSERT_EQ( steps.size(), threeSteps );

        EXPECT_TRUE( std::holds_alternative<grab::overlay::Persistent>(
            added_shape( steps[firstIndex] ).lifetime
        ) );

        const auto& ttl =
            std::get<grab::overlay::Ttl>( added_shape( steps[secondIndex] ).lifetime );
        EXPECT_EQ( ttl.duration, std::chrono::milliseconds{ ttlMilliseconds } );

        const auto& fade =
            std::get<grab::overlay::Fade>( added_shape( steps[thirdIndex] ).lifetime );
        EXPECT_EQ( fade.duration, std::chrono::milliseconds{ fadeMilliseconds } );

        expect_round_trips( overlayLifetimesDocument );
    }

    TEST( Interpreter,
          EveryAnimationChannelLoadsOnItsOwn )
    {
        const auto parsed = parse( overlayAnimationDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto steps = parsed->steps();
        ASSERT_EQ( steps.size(), fourSteps );

        const auto& scaled = added_shape( steps[firstIndex] ).animation;
        ASSERT_TRUE( scaled.has_value() );
        ASSERT_TRUE( scaled->scale.has_value() );
        EXPECT_FALSE( scaled->opacity.has_value() );
        EXPECT_EQ( scaled->scale->easing, grab::overlay::Easing::InQuad );
        EXPECT_EQ( scaled->scale->duration,
                   std::chrono::milliseconds{ channelMilliseconds } );
        EXPECT_EQ( scaled->scale->from, scaleFrom );
        EXPECT_EQ( scaled->scale->to, scaleTo );

        const auto& faded = added_shape( steps[secondIndex] ).animation;
        ASSERT_TRUE( faded.has_value() );
        ASSERT_TRUE( faded->opacity.has_value() );
        EXPECT_EQ( faded->opacity->easing, grab::overlay::Easing::OutCubic );
        EXPECT_EQ( faded->opacity->from, opacityFrom );
        EXPECT_EQ( faded->opacity->to, opacityTo );

        const auto& moved = added_shape( steps[thirdIndex] ).animation;
        ASSERT_TRUE( moved.has_value() );
        ASSERT_TRUE( moved->translate.has_value() );
        EXPECT_EQ( moved->translate->easing, grab::overlay::Easing::Linear );
        EXPECT_EQ( moved->translate->dx, translateDx );
        EXPECT_EQ( moved->translate->dy, translateDy );

        const auto& revealed = added_shape( steps[fourthIndex] ).animation;
        ASSERT_TRUE( revealed.has_value() );
        ASSERT_TRUE( revealed->reveal.has_value() );
        EXPECT_EQ( revealed->reveal->easing, grab::overlay::Easing::InOutCubic );
        EXPECT_EQ( revealed->reveal->axis, grab::overlay::Axis::Y );
        EXPECT_EQ( revealed->reveal->from_edge, grab::overlay::Edge::Max );
        EXPECT_EQ( revealed->reveal->from, revealFrom );
        EXPECT_EQ( revealed->reveal->to, revealTo );

        expect_round_trips( overlayAnimationDocument );
    }

    // ── Overlay handles ──────────────────────────────────────

    TEST( Interpreter,
          AHandleUsedBeforeItsAddIsRejected )
    {
        // Structural, exactly like a dangling `after`: the handle cannot
        // resolve to a ShapeId, so the run would fail on it instead of the
        // document failing to load.
        const std::string message = rejection_of( handleBeforeAddDocument );
        ASSERT_FALSE( message.empty() );
        EXPECT_TRUE( mentions( message, beforeAddPhrase ) ) << message;
        EXPECT_TRUE( mentions( message, liveHandle ) ) << message;
        EXPECT_TRUE( mentions( message, handlePointer ) ) << message;
    }

    TEST( Interpreter,
          AHandleAddedTwiceWhileLiveIsRejected )
    {
        const std::string message = rejection_of( duplicateHandleDocument );
        ASSERT_FALSE( message.empty() );
        EXPECT_TRUE( mentions( message, stillLivePhrase ) ) << message;
        EXPECT_TRUE( mentions( message, liveHandle ) ) << message;
        EXPECT_TRUE( mentions( message, secondHandlePointer ) ) << message;
    }

    TEST( Interpreter,
          AHandleReusedAfterItsRemoveIsLegal )
    {
        const auto parsed = parse( reusedHandleDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;
        ASSERT_EQ( parsed->steps().size(), threeSteps );

        expect_round_trips( reusedHandleDocument );
    }

    TEST( Interpreter,
          AnAddWithNoHandleIsLegalAndWritesNoHandle )
    {
        const auto parsed = parse( unstyledShapeDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto& add = std::get<grab::sequence::OverlayAddCommand>(
            parsed->steps()[firstIndex].command
        );
        EXPECT_TRUE( add.handle.empty() );

        // Fire-and-forget: drawable, never referenced again, and no handle key
        // invented for it on the way out.
        const auto text = to_json( *parsed );
        ASSERT_TRUE( text.has_value() ) << text.error().message;
        EXPECT_FALSE( mentions( *text, handleKeyText ) ) << *text;
    }

    TEST( Interpreter,
          OverlayPayloadsSurviveTheRoundTrip )
    {
        const auto parsed = parse( everyOpDocument );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;

        const auto steps = parsed->steps();
        ASSERT_EQ( steps.size(), everyOpStepCount );

        const auto& update = std::get<grab::sequence::OverlayUpdateCommand>(
            steps[overlayUpdateIndex].command
        );
        EXPECT_EQ( update.handle, liveHandle );
        EXPECT_EQ( update.shape.band, grab::overlay::Band::Trail );
        EXPECT_EQ( std::get<grab::overlay::Ttl>( update.shape.lifetime ).duration,
                   std::chrono::milliseconds{ ttlMilliseconds } );

        const auto& attach = std::get<grab::sequence::OverlayAttachCommand>(
            steps[overlayAttachIndex].command
        );
        ASSERT_TRUE( attach.offset.has_value() );
        EXPECT_EQ( attach.offset->x, attachOffsetX );
        EXPECT_EQ( attach.offset->y, attachOffsetY );

        // overlay.clear, grab and release carry no payload at all, so the only
        // thing to preserve is which one they are.
        EXPECT_EQ( grab::sequence::kind_of( steps[overlayClearIndex].command ),
                   grab::CommandKind::OverlayClear );
    }

    // ── load() ───────────────────────────────────────────────

    TEST( Interpreter,
          LoadReadsADocumentFromDisk )
    {
        const auto path = scratch_file( sequenceFileName );
        {
            std::ofstream output{ path, std::ios::binary };
            ASSERT_TRUE( output.is_open() ) << path.string();
            output << specExample;
        }

        const auto      parsed = load( path );
        std::error_code error;
        std::filesystem::remove( path, error );

        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;
        EXPECT_EQ( parsed->steps().size(), fiveSteps );
    }

    TEST( Interpreter,
          LoadNamesTheFileAndThePointerInAnError )
    {
        const auto path = scratch_file( badFileName );
        {
            std::ofstream output{ path, std::ios::binary };
            ASSERT_TRUE( output.is_open() ) << path.string();
            output << unknownOpDocument;
        }

        const auto      parsed = load( path );
        std::error_code error;
        std::filesystem::remove( path, error );

        ASSERT_FALSE( parsed.has_value() );
        const auto& message = parsed.error().message;
        EXPECT_TRUE( mentions( message, path.string() ) ) << message;
        EXPECT_TRUE( mentions( message, opPointer ) ) << message;
        EXPECT_TRUE( mentions( message, unknownOpName ) ) << message;
    }

    TEST( Interpreter,
          LoadOfAMissingFileSaysSo )
    {
        const auto      path = scratch_file( missingFileName );
        std::error_code error;
        std::filesystem::remove( path, error );

        const auto parsed = load( path );
        ASSERT_FALSE( parsed.has_value() );
        EXPECT_TRUE( mentions( parsed.error().message, missingFilePhrase ) )
            << parsed.error().message;
    }

}    // namespace
