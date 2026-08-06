#include "grab/command.hpp"
#include "grab/command_descriptor.hpp"
#include "grab/drag.hpp"
#include "grab/geometry/curve.hpp"
#include "grab/geometry/point.hpp"
#include "grab/overlay.hpp"
#include "grab/pointer_button.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "grab/space.hpp"
#include "kernel/sequence/interpreter.hpp"
#include "kernel/sequence/sequence.hpp"
#include "kernel/support/log.hpp"
#include "kernel/support/log_tags.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>    // IWYU pragma: keep
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace grab::kernel::sequence
{

    namespace
    {

        // ordered_json rather than json so the emitted document keeps the order
        // the writer used. Nothing depends on it for correctness — identity is
        // compared over the Sequence, not over the bytes — but a serialized
        // step that reads `id, op, payload, after` is the one a human can
        // diff against the one they wrote.
        using Json                                        = nlohmann::ordered_json;

        constexpr std::int64_t     supportedSchemaVersion = 1;
        constexpr int              jsonIndentWidth        = 2;
        constexpr std::string_view rootPointer            = "/";
        constexpr std::string_view documentBase           = "";
        constexpr std::string_view gotoPrefix             = "goto:";
        constexpr std::size_t      pointComponentCount    = 2U;
        constexpr std::size_t      xComponent             = 0U;
        constexpr std::size_t      minimumCurvePoints     = 2U;

        // A step that waits, dwells or breathes for longer than a day is a
        // typo, not an intention. Bounding it here keeps a stray extra zero
        // from becoming a run that never ends, and keeps every millisecond
        // field convertible to nanoseconds without overflow.
        constexpr std::int64_t     maximumDurationMs = 86'400'000;
        constexpr std::int64_t     maximumDurationNs = 86'400'000'000'000;
        constexpr std::int64_t     zeroDuration      = 0;

        constexpr std::int64_t     coordinateMinimum =
            std::numeric_limits<std::int32_t>::min();
        constexpr std::int64_t coordinateMaximum =
            std::numeric_limits<std::int32_t>::max();
        constexpr std::int64_t minimumButtonCode = 1;
        constexpr std::int64_t maximumButtonCode =
            std::numeric_limits<std::uint8_t>::max();

        constexpr std::string_view fieldSchemaVersion = "schema_version";
        constexpr std::string_view fieldSequence      = "sequence";
        constexpr std::string_view fieldPacing        = "pacing";
        constexpr std::string_view fieldSteps         = "steps";
        constexpr std::string_view fieldMode          = "mode";
        constexpr std::string_view fieldGraceMs       = "grace_ms";
        constexpr std::string_view fieldId            = "id";
        constexpr std::string_view fieldOp            = "op";
        constexpr std::string_view fieldAfter         = "after";
        constexpr std::string_view fieldOnError       = "on_error";
        constexpr std::string_view fieldExtraGraceMs  = "extra_grace_ms";
        constexpr std::string_view fieldText          = "text";
        constexpr std::string_view fieldKey           = "key";
        constexpr std::string_view fieldButton        = "button";
        constexpr std::string_view fieldAt            = "at";
        constexpr std::string_view fieldTo            = "to";
        constexpr std::string_view fieldFrom          = "from";
        constexpr std::string_view fieldDx            = "dx";
        constexpr std::string_view fieldDy            = "dy";
        constexpr std::string_view fieldCurve         = "curve";
        constexpr std::string_view fieldOptions       = "options";
        constexpr std::string_view fieldStepDwellMs   = "step_dwell_ms";
        constexpr std::string_view fieldPath          = "path";
        constexpr std::string_view fieldOut           = "out";
        constexpr std::string_view fieldLocator       = "locator";
        constexpr std::string_view fieldMs            = "ms";
        constexpr std::string_view fieldNs            = "ns";

        // ── Overlay steps, design §3.2 ───────────────────────
        //
        // `handle` is a DOCUMENT-LEVEL NAME, exactly like a step label: the
        // run state maps it to an overlay::ShapeId, and it is never written
        // out as one.
        //
        // `path` is deliberately the same spelling as DragOptions::path. The
        // two never meet — one is a shape's command list inside `shape`, the
        // other a curve kind inside `options` — and giving a shape's path a
        // different name would be the surprise.
        constexpr std::string_view fieldHandle     = "handle";
        constexpr std::string_view fieldShape      = "shape";
        constexpr std::string_view fieldOffset     = "offset";
        constexpr std::string_view fieldRect       = "rect";
        constexpr std::string_view fieldEllipse    = "ellipse";
        constexpr std::string_view fieldPolygon    = "polygon";
        constexpr std::string_view fieldX          = "x";
        constexpr std::string_view fieldY          = "y";
        constexpr std::string_view fieldW          = "w";
        constexpr std::string_view fieldH          = "h";
        constexpr std::string_view fieldCenter     = "center";
        constexpr std::string_view fieldRadius     = "radius";
        constexpr std::string_view fieldRadiusX    = "radius_x";
        constexpr std::string_view fieldRadiusY    = "radius_y";
        constexpr std::string_view fieldMove       = "move";
        constexpr std::string_view fieldLine       = "line";
        constexpr std::string_view fieldBezier     = "bezier";
        constexpr std::string_view fieldCommands   = "commands";
        constexpr std::string_view fieldClosed     = "closed";
        constexpr std::string_view fieldStroke     = "stroke";
        constexpr std::string_view fieldFill       = "fill";
        constexpr std::string_view fieldColor      = "color";
        constexpr std::string_view fieldWidth      = "width";
        constexpr std::string_view fieldLifetime   = "lifetime";
        constexpr std::string_view fieldTtlMs      = "ttl_ms";
        constexpr std::string_view fieldFadeMs     = "fade_ms";
        constexpr std::string_view fieldBand       = "band";
        constexpr std::string_view fieldZ          = "z";
        constexpr std::string_view fieldAnimation  = "animation";
        constexpr std::string_view fieldScale      = "scale";
        constexpr std::string_view fieldOpacity    = "opacity";
        constexpr std::string_view fieldTranslate  = "translate";
        constexpr std::string_view fieldReveal     = "reveal";
        constexpr std::string_view fieldEasing     = "easing";
        constexpr std::string_view fieldDurationMs = "duration_ms";
        constexpr std::string_view fieldAxis       = "axis";
        constexpr std::string_view fieldFromEdge   = "from_edge";

        // The only two bare strings in the shape grammar: ClosePath is the one
        // path command with no operand, Persistent the one lifetime with no
        // duration.
        constexpr std::string_view closeCommandText      = "close";
        constexpr std::string_view persistentText        = "persistent";

        constexpr std::size_t      minimumPolygonPoints  = 3U;
        constexpr std::size_t      minimumBezierControls = 1U;
        constexpr std::int64_t     defaultZIndex         = 0;
        constexpr std::int64_t  zIndexMinimum = std::numeric_limits<std::int32_t>::min();
        constexpr std::int64_t  zIndexMaximum = std::numeric_limits<std::int32_t>::max();

        // Colours take two forms and no others. This is the one place the
        // codec is deliberately strict about spelling: a mis-parsed colour is
        // invisible in a test and obvious only on a screen nobody is watching.
        constexpr std::size_t   rgbTextLength     = 7U;    // #rrggbb
        constexpr std::size_t   rgbaTextLength    = 9U;    // #rrggbbaa
        constexpr std::size_t   colorPrefixLength = 1U;
        constexpr char          colorPrefix       = '#';
        constexpr std::size_t   hexDigitsPerByte  = 2U;
        constexpr std::uint32_t hexadecimalBase   = 16U;
        constexpr std::uint8_t  hexAlphaOffset    = 10U;
        constexpr std::uint8_t  hexHighShift      = 4U;
        constexpr std::uint8_t  hexLowMask        = 0X0FU;
        constexpr std::uint8_t  opaqueAlpha  = std::numeric_limits<std::uint8_t>::max();
        constexpr std::size_t   redChannel   = 0U;
        constexpr std::size_t   greenChannel = 1U;
        constexpr std::size_t   blueChannel  = 2U;
        constexpr std::size_t   alphaChannel = 3U;
        constexpr std::size_t   colorChannelCount = 4U;
        constexpr std::string_view hexDigitText   = "0123456789abcdef";
        constexpr std::string_view colorForms     = "expected '#rrggbb' or '#rrggbbaa'";

        struct ButtonName
        {
                std::string_view text;
                std::uint8_t     code;
                // Exactly one spelling per code is written back out; the rest
                // are accepted aliases, so "primary" loads and "left" is what
                // to_json emits.
                bool             canonical;
        };

        constexpr auto buttonNames = std::to_array<ButtonName>( {
            ButtonName{
                       .text = "left",
                       .code = grab::input::button_code( grab::input::PointerButton::Primary ),
                       .canonical = true },
            ButtonName{
                       .text = "primary",
                       .code = grab::input::button_code( grab::input::PointerButton::Primary ),
                       .canonical = false},
            ButtonName{
                       .text = "middle",
                       .code = grab::input::button_code( grab::input::PointerButton::Middle ),
                       .canonical = true },
            ButtonName{
                       .text = "right",
                       .code =
                       grab::input::button_code( grab::input::PointerButton::Secondary ),
                       .canonical = true },
            ButtonName{
                       .text = "secondary",
                       .code =
                       grab::input::button_code( grab::input::PointerButton::Secondary ),
                       .canonical = false},
            ButtonName{
                       .text = "wheel_up",
                       .code = grab::input::button_code( grab::input::PointerButton::WheelUp ),
                       .canonical = true },
            ButtonName{
                       .text = "wheel_down",
                       .code =
                       grab::input::button_code( grab::input::PointerButton::WheelDown ),
                       .canonical = true },
            ButtonName{
                       .text = "wheel_left",
                       .code =
                       grab::input::button_code( grab::input::PointerButton::WheelLeft ),
                       .canonical = true },
            ButtonName{
                       .text = "wheel_right",
                       .code =
                       grab::input::button_code( grab::input::PointerButton::WheelRight ),
                       .canonical = true },
        } );

        struct PathName
        {
                std::string_view               text;
                grab::input::DragOptions::Path value;
        };

        constexpr auto pathNames = std::to_array<PathName>( {
            PathName{
                     .text  = "linear",
                     .value = grab::input::DragOptions::Path::Linear                 },
            PathName{ .text = "cubic", .value = grab::input::DragOptions::Path::Cubic},
        } );

        // Every enum the shape grammar spells, in the snake_case the button
        // names already established. One entry per enumerator: a missing one
        // is not a silently-defaulted shape, it is a rejection naming the
        // whole accepted set.
        struct BandName
        {
                std::string_view    text;
                grab::overlay::Band value;
        };

        constexpr auto bandNames = std::to_array<BandName>( {
            BandName{.text = "annotation", .value = grab::overlay::Band::Annotation},
            BandName{     .text = "trail",      .value = grab::overlay::Band::Trail},
        } );

        struct EasingName
        {
                std::string_view      text;
                grab::overlay::Easing value;
        };

        constexpr auto easingNames = std::to_array<EasingName>( {
            EasingName{.text = "linear",.value = grab::overlay::Easing::Linear                                              },
            EasingName{     .text = "in_quad",    .value = grab::overlay::Easing::InQuad},
            EasingName{    .text = "out_quad",   .value = grab::overlay::Easing::OutQuad},
            EasingName{
                       .text  = "in_out_quad",
                       .value = grab::overlay::Easing::InOutQuad                        },
            EasingName{    .text = "in_cubic",   .value = grab::overlay::Easing::InCubic},
            EasingName{   .text = "out_cubic",  .value = grab::overlay::Easing::OutCubic},
            EasingName{
                       .text  = "in_out_cubic",
                       .value = grab::overlay::Easing::InOutCubic                       },
        } );

        struct AxisName
        {
                std::string_view    text;
                grab::overlay::Axis value;
        };

        constexpr auto axisNames = std::to_array<AxisName>( {
            AxisName{.text = "x", .value = grab::overlay::Axis::X},
            AxisName{.text = "y", .value = grab::overlay::Axis::Y},
        } );

        struct EdgeName
        {
                std::string_view    text;
                grab::overlay::Edge value;
        };

        constexpr auto             edgeNames   = std::to_array<EdgeName>( {
            EdgeName{.text = "min", .value = grab::overlay::Edge::Min},
            EdgeName{.text = "max", .value = grab::overlay::Edge::Max},
        } );

        constexpr std::string_view bandForms   = "expected annotation or trail";
        constexpr std::string_view easingForms = "expected linear, in_quad, out_quad, "
                                                 "in_out_quad, in_cubic, out_cubic or "
                                                 "in_out_cubic";
        constexpr std::string_view axisForms   = "expected x or y";
        constexpr std::string_view edgeForms   = "expected min or max";

        // ── Pointers and errors ──────────────────────────────

        // Every pointer token here is either a fixed field name or a decimal
        // index, so none of them can contain the `~` or `/` that RFC 6901
        // escaping exists for.
        [[nodiscard]]
        std::string
        child_pointer( std::string_view base,
                       std::string_view token )
        {
            std::string pointer{ base };
            pointer.push_back( '/' );
            pointer.append( token );
            return pointer;
        }

        [[nodiscard]]
        std::string
        element_pointer( std::string_view base,
                         std::size_t      index )
        {
            return child_pointer( base, std::to_string( index ) );
        }

        [[nodiscard]]
        std::string
        step_pointer( std::size_t index )
        {
            return element_pointer( child_pointer( documentBase, fieldSteps ), index );
        }

        // How a step is named in a message: the author's label where there is
        // one, its document position otherwise. A document may be entirely
        // unlabelled, so a label alone cannot locate a fault.
        [[nodiscard]]
        std::string
        step_subject( std::string_view label,
                      std::size_t      index )
        {
            if( label.empty() )
            {
                std::string subject{ "step at index " };
                subject.append( std::to_string( index ) );
                return subject;
            }
            std::string subject{ "step '" };
            subject.append( label );
            subject.push_back( '\'' );
            return subject;
        }

        // `origin` is empty for parse() and "<path>: " for load(), so one
        // message shape covers both entry points. `subject` is empty at
        // document scope and a step designation inside a step.
        struct Scope
        {
                std::string_view origin{};
                std::string      pointer{};
                std::string      subject{};
        };

        [[nodiscard]]
        std::unexpected<grab::Error>
        reject( const Scope&     scope,
                std::string_view pointer,
                std::string_view reason )
        {
            std::string message{ scope.origin };
            message.append( pointer );
            message.append( ": " );
            if( !scope.subject.empty() )
            {
                message.append( scope.subject );
                message.append( ": " );
            }
            message.append( reason );
            return grab::fail( grab::ErrorCode::InvalidArgument, std::move( message ) );
        }

        [[nodiscard]]
        std::string
        json_key( std::string_view field )
        {
            return std::string{ field };
        }

        [[nodiscard]]
        constexpr grab::sequence::StepId
        step_id_at( std::size_t index ) noexcept
        {
            return grab::sequence::StepId{
                static_cast<grab::sequence::StepId::Half>( index ),
                grab::sequence::StepId::firstGeneration
            };
        }

        // ── Field readers ────────────────────────────────────

        [[nodiscard]]
        grab::Result<std::string>
        require_string( const Scope&     scope,
                        const Json&      node,
                        std::string_view field )
        {
            const std::string name    = json_key( field );
            const std::string pointer = child_pointer( scope.pointer, field );
            if( !node.contains( name ) )
            {
                return reject( scope, pointer, "missing required field" );
            }
            const Json& value = node.at( name );
            if( !value.is_string() )
            {
                return reject( scope, pointer, "must be a string" );
            }
            return value.get<std::string>();
        }

        [[nodiscard]]
        grab::Result<std::int64_t>
        bounded_integer( const Scope&       scope,
                         const std::string& pointer,
                         const Json&        value,
                         std::int64_t       low,
                         std::int64_t       high )
        {
            const auto out_of_range = [&scope, &pointer, low, high]()
            {
                std::string reason{ "must be an integer between " };
                reason.append( std::to_string( low ) );
                reason.append( " and " );
                reason.append( std::to_string( high ) );
                return reject( scope, pointer, reason );
            };

            if( !value.is_number_integer() )
            {
                return reject( scope, pointer, "must be an integer" );
            }
            if( value.is_number_unsigned() &&
                value.get<std::uint64_t>() >
                static_cast<std::uint64_t>( std::numeric_limits<std::int64_t>::max() ) )
            {
                return out_of_range();
            }
            const auto raw = value.get<std::int64_t>();
            if( raw < low || raw > high )
            {
                return out_of_range();
            }
            return raw;
        }

        [[nodiscard]]
        grab::Result<std::int64_t>
        optional_integer( const Scope&     scope,
                          const Json&      node,
                          std::string_view field,
                          std::int64_t     fallback,
                          std::int64_t     low,
                          std::int64_t     high )
        {
            const std::string name = json_key( field );
            if( !node.contains( name ) )
            {
                return fallback;
            }
            return bounded_integer( scope,
                                    child_pointer( scope.pointer, field ),
                                    node.at( name ),
                                    low,
                                    high );
        }

        [[nodiscard]]
        grab::Result<grab::geometry::Point>
        read_point( const Scope&       scope,
                    const std::string& pointer,
                    const Json&        value )
        {
            if( !value.is_array() || value.size() != pointComponentCount )
            {
                return reject( scope, pointer, "must be a two-element [x, y] array" );
            }
            grab::geometry::Point point{};
            for( std::size_t axis = 0U; axis < pointComponentCount; ++axis )
            {
                const auto component = bounded_integer( scope,
                                                        element_pointer( pointer, axis ),
                                                        value.at( axis ),
                                                        coordinateMinimum,
                                                        coordinateMaximum );
                if( !component.has_value() )
                {
                    return std::unexpected( component.error() );
                }
                if( axis == xComponent )
                {
                    point.x = static_cast<std::int32_t>( *component );
                }
                else
                {
                    point.y = static_cast<std::int32_t>( *component );
                }
            }
            return point;
        }

        [[nodiscard]]
        grab::Result<grab::geometry::Point>
        require_point( const Scope&     scope,
                       const Json&      node,
                       std::string_view field )
        {
            const std::string name    = json_key( field );
            const std::string pointer = child_pointer( scope.pointer, field );
            if( !node.contains( name ) )
            {
                return reject( scope, pointer, "missing required field" );
            }
            return read_point( scope, pointer, node.at( name ) );
        }

        [[nodiscard]]
        grab::Result<std::optional<grab::geometry::Point>>
        optional_point( const Scope&     scope,
                        const Json&      node,
                        std::string_view field )
        {
            const std::string name = json_key( field );
            if( !node.contains( name ) )
            {
                return std::optional<grab::geometry::Point>{};
            }
            const auto point = read_point( scope,
                                           child_pointer( scope.pointer, field ),
                                           node.at( name ) );
            if( !point.has_value() )
            {
                return std::unexpected( point.error() );
            }
            return std::optional<grab::geometry::Point>{ *point };
        }

        [[nodiscard]]
        grab::Result<std::uint8_t>
        require_button( const Scope& scope,
                        const Json&  node )
        {
            const std::string name    = json_key( fieldButton );
            const std::string pointer = child_pointer( scope.pointer, fieldButton );
            if( !node.contains( name ) )
            {
                return grab::input::primaryButton;
            }
            const Json& value = node.at( name );
            if( value.is_string() )
            {
                const auto        text = value.get<std::string>();
                const auto* const found =
                    std::ranges::find( buttonNames, text, &ButtonName::text );
                if( found == buttonNames.end() )
                {
                    std::string reason{ "unknown button '" };
                    reason.append( text );
                    reason.append( "'; expected left, middle, right, a wheel "
                                   "direction, or a code in 1..255" );
                    return reject( scope, pointer, reason );
                }
                return found->code;
            }
            const auto code = bounded_integer( scope,
                                               pointer,
                                               value,
                                               minimumButtonCode,
                                               maximumButtonCode );
            if( !code.has_value() )
            {
                return std::unexpected( code.error() );
            }
            return static_cast<std::uint8_t>( *code );
        }

        [[nodiscard]]
        grab::Result<grab::input::DragOptions>
        require_options( const Scope& scope,
                         const Json&  node )
        {
            grab::input::DragOptions options{};
            const std::string        name = json_key( fieldOptions );
            const std::string pointer     = child_pointer( scope.pointer, fieldOptions );
            if( !node.contains( name ) )
            {
                return options;
            }
            const Json& value = node.at( name );
            if( !value.is_object() )
            {
                return reject( scope, pointer, "must be an object" );
            }

            const Scope inner{
                .origin  = scope.origin,
                .pointer = pointer,
                .subject = scope.subject,
            };

            const auto steps =
                optional_integer( inner,
                                  value,
                                  fieldSteps,
                                  grab::input::DragOptions::defaultInterpolationSteps,
                                  grab::input::DragOptions::minimumInterpolationSteps,
                                  grab::input::DragOptions::maximumInterpolationSteps );
            if( !steps.has_value() )
            {
                return std::unexpected( steps.error() );
            }
            options.interpolation_steps = static_cast<std::int32_t>( *steps );

            const auto dwell =
                optional_integer( inner,
                                  value,
                                  fieldStepDwellMs,
                                  static_cast<std::int64_t>(
                                      grab::input::DragOptions::defaultStepDwell.count()
                                  ),
                                  zeroDuration,
                                  maximumDurationMs );
            if( !dwell.has_value() )
            {
                return std::unexpected( dwell.error() );
            }
            options.step_dwell         = std::chrono::milliseconds{ *dwell };

            const std::string pathName = json_key( fieldPath );
            if( value.contains( pathName ) )
            {
                const auto text = require_string( inner, value, fieldPath );
                if( !text.has_value() )
                {
                    return std::unexpected( text.error() );
                }
                const auto* const found =
                    std::ranges::find( pathNames, *text, &PathName::text );
                if( found == pathNames.end() )
                {
                    std::string reason{ "unknown path '" };
                    reason.append( *text );
                    reason.append( "'; expected linear or cubic" );
                    return reject( inner, child_pointer( pointer, fieldPath ), reason );
                }
                options.path = found->value;
            }
            return options;
        }

        [[nodiscard]]
        grab::Result<grab::geometry::Curve>
        require_curve( const Scope& scope,
                       const Json&  node )
        {
            const std::string name    = json_key( fieldCurve );
            const std::string pointer = child_pointer( scope.pointer, fieldCurve );
            if( !node.contains( name ) )
            {
                return reject( scope, pointer, "missing required field" );
            }
            const Json& value = node.at( name );
            if( !value.is_array() || value.size() < minimumCurvePoints )
            {
                return reject(
                    scope,
                    pointer,
                    "must be an array of at least two [x, y] control points"
                );
            }

            grab::geometry::Curve curve{};
            curve.control.reserve( value.size() );
            for( std::size_t index = 0U; index < value.size(); ++index )
            {
                const std::string controlPointer = element_pointer( pointer, index );
                const Json&       control        = value.at( index );
                if( !control.is_array() || control.size() != pointComponentCount )
                {
                    return reject( scope,
                                   controlPointer,
                                   "must be a two-element [x, y] array" );
                }
                grab::geometry::PointF point{};
                for( std::size_t axis = 0U; axis < pointComponentCount; ++axis )
                {
                    const Json& component = control.at( axis );
                    if( !component.is_number() )
                    {
                        return reject( scope,
                                       element_pointer( controlPointer, axis ),
                                       "must be a number" );
                    }
                    if( axis == xComponent )
                    {
                        point.x = component.get<double>();
                    }
                    else
                    {
                        point.y = component.get<double>();
                    }
                }
                curve.control.push_back( point );
            }
            return curve;
        }

        // ── Overlay shapes ───────────────────────────────────

        [[nodiscard]]
        Scope
        nested( const Scope& scope,
                std::string  pointer )
        {
            return Scope{
                .origin  = scope.origin,
                .pointer = std::move( pointer ),
                .subject = scope.subject,
            };
        }

        // One reader for every "a name out of a fixed set" field: band,
        // easing, axis, from_edge. An unknown name names the whole accepted
        // set rather than defaulting silently, because a defaulted easing
        // looks exactly like a working one.
        template<typename EntryT>
        [[nodiscard]]
        grab::Result<decltype( EntryT::value )>
        optional_enum( const Scope&              scope,
                       const Json&               node,
                       std::string_view          field,
                       std::span<const EntryT>   table,
                       decltype( EntryT::value ) fallback,
                       std::string_view          what,
                       std::string_view          forms )
        {
            const std::string name    = json_key( field );
            const std::string pointer = child_pointer( scope.pointer, field );
            if( !node.contains( name ) )
            {
                return fallback;
            }
            const Json& value = node.at( name );
            if( !value.is_string() )
            {
                std::string reason{ "must be a " };
                reason.append( what );
                reason.append( " name; " );
                reason.append( forms );
                return reject( scope, pointer, reason );
            }
            const auto text  = value.get<std::string>();
            const auto found = std::ranges::find( table, text, &EntryT::text );
            if( found == table.end() )
            {
                std::string reason{ "unknown " };
                reason.append( what );
                reason.append( " '" );
                reason.append( text );
                reason.append( "'; " );
                reason.append( forms );
                return reject( scope, pointer, reason );
            }
            return found->value;
        }

        [[nodiscard]]
        constexpr std::optional<std::uint8_t>
        hex_digit( char text ) noexcept
        {
            if( text >= '0' && text <= '9' )
            {
                return static_cast<std::uint8_t>( text - '0' );
            }
            if( text >= 'a' && text <= 'f' )
            {
                return static_cast<std::uint8_t>( text - 'a' + hexAlphaOffset );
            }
            if( text >= 'A' && text <= 'F' )
            {
                return static_cast<std::uint8_t>( text - 'A' + hexAlphaOffset );
            }
            return std::nullopt;
        }

        // #rrggbb and #rrggbbaa, and nothing else. Three channels mean opaque,
        // which is what a three-channel colour means everywhere else in grab.
        [[nodiscard]]
        std::optional<grab::overlay::Color>
        color_from_text( std::string_view text ) noexcept
        {
            if( text.size() != rgbTextLength && text.size() != rgbaTextLength )
            {
                return std::nullopt;
            }
            if( text.front() != colorPrefix )
            {
                return std::nullopt;
            }

            std::array<std::uint8_t, colorChannelCount>
                              channels{ 0U, 0U, 0U, opaqueAlpha };
            const std::size_t present =
                ( text.size() - colorPrefixLength ) / hexDigitsPerByte;
            for( std::size_t channel = 0U; channel < present; ++channel )
            {
                const std::size_t offset =
                    colorPrefixLength + ( channel * hexDigitsPerByte );
                const auto high = hex_digit( text[offset] );
                const auto low  = hex_digit( text[offset + 1U] );
                if( !high.has_value() || !low.has_value() )
                {
                    return std::nullopt;
                }
                channels[channel] = static_cast<std::uint8_t>(
                    ( static_cast<std::uint32_t>( *high ) * hexadecimalBase ) + *low
                );
            }
            return grab::overlay::Color{
                .r = channels[redChannel],
                .g = channels[greenChannel],
                .b = channels[blueChannel],
                .a = channels[alphaChannel],
            };
        }

        [[nodiscard]]
        std::string
        color_text( grab::overlay::Color color )
        {
            std::string text{ colorPrefix };
            const auto  append = [&text]( std::uint8_t value )
            {
                text.push_back(
                    hexDigitText[static_cast<std::size_t>( value >> hexHighShift )]
                );
                text.push_back(
                    hexDigitText[static_cast<std::size_t>( value & hexLowMask )]
                );
            };
            append( color.r );
            append( color.g );
            append( color.b );
            // The short form when there is nothing to say about alpha, so a
            // document written as #4ecea9 round-trips to itself.
            if( color.a != opaqueAlpha )
            {
                append( color.a );
            }
            return text;
        }

        [[nodiscard]]
        grab::Result<grab::overlay::Color>
        require_color( const Scope&     scope,
                       const Json&      node,
                       std::string_view field )
        {
            const std::string name    = json_key( field );
            const std::string pointer = child_pointer( scope.pointer, field );
            if( !node.contains( name ) )
            {
                std::string reason{ "missing required field; " };
                reason.append( colorForms );
                return reject( scope, pointer, reason );
            }
            const Json& value = node.at( name );
            if( !value.is_string() )
            {
                std::string reason{ "must be a color string; " };
                reason.append( colorForms );
                return reject( scope, pointer, reason );
            }
            const auto text  = value.get<std::string>();
            const auto color = color_from_text( text );
            if( !color.has_value() )
            {
                std::string reason{ "invalid color '" };
                reason.append( text );
                reason.append( "'; " );
                reason.append( colorForms );
                return reject( scope, pointer, reason );
            }
            return *color;
        }

        [[nodiscard]]
        grab::Result<double>
        read_number( const Scope&       scope,
                     const std::string& pointer,
                     const Json&        value )
        {
            if( !value.is_number() )
            {
                return reject( scope, pointer, "must be a number" );
            }
            return value.get<double>();
        }

        [[nodiscard]]
        grab::Result<double>
        require_number( const Scope&     scope,
                        const Json&      node,
                        std::string_view field )
        {
            const std::string name    = json_key( field );
            const std::string pointer = child_pointer( scope.pointer, field );
            if( !node.contains( name ) )
            {
                return reject( scope, pointer, "missing required field" );
            }
            return read_number( scope, pointer, node.at( name ) );
        }

        [[nodiscard]]
        grab::Result<double>
        optional_number( const Scope&     scope,
                         const Json&      node,
                         std::string_view field,
                         double           fallback )
        {
            const std::string name = json_key( field );
            if( !node.contains( name ) )
            {
                return fallback;
            }
            return read_number( scope,
                                child_pointer( scope.pointer, field ),
                                node.at( name ) );
        }

        [[nodiscard]]
        grab::Result<std::int64_t>
        require_integer( const Scope&     scope,
                         const Json&      node,
                         std::string_view field,
                         std::int64_t     low,
                         std::int64_t     high )
        {
            const std::string name    = json_key( field );
            const std::string pointer = child_pointer( scope.pointer, field );
            if( !node.contains( name ) )
            {
                return reject( scope, pointer, "missing required field" );
            }
            return bounded_integer( scope, pointer, node.at( name ), low, high );
        }

        // A negative radius, extent or stroke width is a typo every time: it
        // draws nothing and says nothing, which is exactly the failure this
        // format is worst at surfacing.
        [[nodiscard]]
        grab::Result<double>
        require_extent( const Scope&     scope,
                        const Json&      node,
                        std::string_view field )
        {
            const auto value = require_number( scope, node, field );
            if( !value.has_value() )
            {
                return std::unexpected( value.error() );
            }
            if( *value < 0.0 )
            {
                return reject( scope,
                               child_pointer( scope.pointer, field ),
                               "must not be negative" );
            }
            return *value;
        }

        // Shape coordinates are doubles in the overlay's own space, unlike the
        // integer device points input commands carry. The space id itself is
        // run state — a document is written before any session exists — so it
        // is never spelled and always default.
        [[nodiscard]]
        grab::Result<grab::SpacePoint>
        read_space_point( const Scope&       scope,
                          const std::string& pointer,
                          const Json&        value )
        {
            if( !value.is_array() || value.size() != pointComponentCount )
            {
                return reject( scope, pointer, "must be a two-element [x, y] array" );
            }
            grab::SpacePoint point{};
            for( std::size_t axis = 0U; axis < pointComponentCount; ++axis )
            {
                const auto component = read_number( scope,
                                                    element_pointer( pointer, axis ),
                                                    value.at( axis ) );
                if( !component.has_value() )
                {
                    return std::unexpected( component.error() );
                }
                if( axis == xComponent )
                {
                    point.x = *component;
                }
                else
                {
                    point.y = *component;
                }
            }
            return point;
        }

        [[nodiscard]]
        grab::Result<grab::SpacePoint>
        require_space_point( const Scope&     scope,
                             const Json&      node,
                             std::string_view field )
        {
            const std::string name    = json_key( field );
            const std::string pointer = child_pointer( scope.pointer, field );
            if( !node.contains( name ) )
            {
                return reject( scope, pointer, "missing required field" );
            }
            return read_space_point( scope, pointer, node.at( name ) );
        }

        constexpr auto geometryKeys = std::to_array<std::string_view>( {
            fieldRect,
            fieldEllipse,
            fieldPolygon,
            fieldPath,
        } );

        [[nodiscard]]
        grab::Result<grab::overlay::Geometry>
        read_rect( const Scope&       scope,
                   const std::string& pointer,
                   const Json&        value )
        {
            if( !value.is_object() )
            {
                return reject( scope,
                               pointer,
                               "rect must be an object with x, y, w and h" );
            }
            const Scope inner = nested( scope, pointer );

            const auto  x     = require_number( inner, value, fieldX );
            if( !x.has_value() )
            {
                return std::unexpected( x.error() );
            }
            const auto y = require_number( inner, value, fieldY );
            if( !y.has_value() )
            {
                return std::unexpected( y.error() );
            }
            const auto w = require_extent( inner, value, fieldW );
            if( !w.has_value() )
            {
                return std::unexpected( w.error() );
            }
            const auto h = require_extent( inner, value, fieldH );
            if( !h.has_value() )
            {
                return std::unexpected( h.error() );
            }
            return grab::overlay::Geometry{
                grab::overlay::Rect{
                                    .bounds = grab::SpaceRect{
                        .x     = *x,
                        .y     = *y,
                        .w     = *w,
                        .h     = *h,
                        .space = {},
                    }, }
            };
        }

        // `radius` is shorthand for equal radii. Mixing the two spellings is
        // an error rather than a precedence rule: which one wins is exactly
        // the kind of thing nobody should have to remember.
        [[nodiscard]]
        grab::Result<grab::overlay::Geometry>
        read_ellipse( const Scope&       scope,
                      const std::string& pointer,
                      const Json&        value )
        {
            if( !value.is_object() )
            {
                return reject( scope,
                               pointer,
                               "ellipse must be an object with a center and a radius" );
            }
            const Scope inner  = nested( scope, pointer );

            const auto  center = require_space_point( inner, value, fieldCenter );
            if( !center.has_value() )
            {
                return std::unexpected( center.error() );
            }

            const bool has_radius = value.contains( json_key( fieldRadius ) );
            const bool has_x      = value.contains( json_key( fieldRadiusX ) );
            const bool has_y      = value.contains( json_key( fieldRadiusY ) );

            double     radius_x   = 0.0;
            double     radius_y   = 0.0;
            if( has_radius )
            {
                if( has_x || has_y )
                {
                    return reject( inner,
                                   pointer,
                                   "ellipse takes 'radius', or both 'radius_x' and "
                                   "'radius_y', never both spellings" );
                }
                const auto radius = require_extent( inner, value, fieldRadius );
                if( !radius.has_value() )
                {
                    return std::unexpected( radius.error() );
                }
                radius_x = *radius;
                radius_y = *radius;
            }
            else if( has_x && has_y )
            {
                const auto x = require_extent( inner, value, fieldRadiusX );
                if( !x.has_value() )
                {
                    return std::unexpected( x.error() );
                }
                const auto y = require_extent( inner, value, fieldRadiusY );
                if( !y.has_value() )
                {
                    return std::unexpected( y.error() );
                }
                radius_x = *x;
                radius_y = *y;
            }
            else
            {
                return reject( inner,
                               pointer,
                               "ellipse needs 'radius', or both 'radius_x' and "
                               "'radius_y'" );
            }

            return grab::overlay::Geometry{
                grab::overlay::Ellipse{
                                       .center   = *center,
                                       .radius_x = radius_x,
                                       .radius_y = radius_y,
                                       }
            };
        }

        [[nodiscard]]
        grab::Result<grab::overlay::Geometry>
        read_polygon( const Scope&       scope,
                      const std::string& pointer,
                      const Json&        value )
        {
            if( !value.is_array() || value.size() < minimumPolygonPoints )
            {
                return reject( scope,
                               pointer,
                               "polygon must be an array of at least three [x, y] "
                               "points" );
            }
            grab::overlay::Polygon polygon{};
            polygon.points.reserve( value.size() );
            for( std::size_t index = 0U; index < value.size(); ++index )
            {
                const auto point = read_space_point( scope,
                                                     element_pointer( pointer, index ),
                                                     value.at( index ) );
                if( !point.has_value() )
                {
                    return std::unexpected( point.error() );
                }
                polygon.points.push_back( *point );
            }
            return grab::overlay::Geometry{ std::move( polygon ) };
        }

        // "close" is the bare-string form of ClosePath, the only path command
        // with no operand; everything else is a single-key object.
        [[nodiscard]]
        grab::Result<grab::overlay::PathCommand>
        read_path_command( const Scope&       scope,
                           const std::string& pointer,
                           const Json&        value )
        {
            if( value.is_string() )
            {
                const auto text = value.get<std::string>();
                if( text != closeCommandText )
                {
                    std::string reason{ "unknown path command '" };
                    reason.append( text );
                    reason.append( "'; \"close\" is the only bare-string command" );
                    return reject( scope, pointer, reason );
                }
                return grab::overlay::PathCommand{ grab::overlay::ClosePath{} };
            }
            if( !value.is_object() )
            {
                return reject( scope,
                               pointer,
                               "must be \"close\" or a single-key object naming "
                               "move, line or bezier" );
            }

            const bool has_move   = value.contains( json_key( fieldMove ) );
            const bool has_line   = value.contains( json_key( fieldLine ) );
            const bool has_bezier = value.contains( json_key( fieldBezier ) );
            const auto present    = static_cast<std::size_t>( has_move ) +
                                    static_cast<std::size_t>( has_line ) +
                                    static_cast<std::size_t>( has_bezier );
            if( present != 1U )
            {
                return reject( scope,
                               pointer,
                               "must carry exactly one of 'move', 'line' and "
                               "'bezier'" );
            }

            const Scope inner = nested( scope, pointer );
            if( has_move )
            {
                const auto point = require_space_point( inner, value, fieldMove );
                if( !point.has_value() )
                {
                    return std::unexpected( point.error() );
                }
                return grab::overlay::PathCommand{
                    grab::overlay::MoveTo{ .point = *point }
                };
            }
            if( has_line )
            {
                const auto point = require_space_point( inner, value, fieldLine );
                if( !point.has_value() )
                {
                    return std::unexpected( point.error() );
                }
                return grab::overlay::PathCommand{
                    grab::overlay::LineTo{ .point = *point }
                };
            }

            const std::string controlPointer = child_pointer( pointer, fieldBezier );
            const Json&       controls       = value.at( json_key( fieldBezier ) );
            if( !controls.is_array() || controls.size() < minimumBezierControls )
            {
                return reject( scope,
                               controlPointer,
                               "bezier must be an array of at least one [x, y] "
                               "control point" );
            }
            grab::overlay::BezierTo bezier{};
            bezier.control.reserve( controls.size() );
            for( std::size_t index = 0U; index < controls.size(); ++index )
            {
                const auto point =
                    read_space_point( scope,
                                      element_pointer( controlPointer, index ),
                                      controls.at( index ) );
                if( !point.has_value() )
                {
                    return std::unexpected( point.error() );
                }
                bezier.control.push_back( *point );
            }
            return grab::overlay::PathCommand{ std::move( bezier ) };
        }

        [[nodiscard]]
        grab::Result<grab::overlay::Path>
        read_path_commands( const Scope&       scope,
                            const std::string& pointer,
                            const Json&        value )
        {
            if( !value.is_array() )
            {
                return reject( scope, pointer, "must be an array of path commands" );
            }
            grab::overlay::Path path{};
            path.commands.reserve( value.size() );
            for( std::size_t index = 0U; index < value.size(); ++index )
            {
                auto command = read_path_command( scope,
                                                  element_pointer( pointer, index ),
                                                  value.at( index ) );
                if( !command.has_value() )
                {
                    return std::unexpected( command.error() );
                }
                path.commands.push_back( std::move( *command ) );
            }
            return path;
        }

        // §3.2 spells a path as a bare array of commands, and that array is
        // the only form to_json emits. It cannot carry overlay::Path::closed —
        // the flag that closes the LAST contour, as distinct from a ClosePath
        // command closing the active one — so the object form exists for the
        // one case the array cannot spell: a Path built in C++ with
        // closed = true. A to_json that cannot write a value its own type
        // holds is a hole in the round trip, not a simplification.
        [[nodiscard]]
        grab::Result<grab::overlay::Geometry>
        read_path( const Scope&       scope,
                   const std::string& pointer,
                   const Json&        value )
        {
            if( value.is_array() )
            {
                auto path = read_path_commands( scope, pointer, value );
                if( !path.has_value() )
                {
                    return std::unexpected( path.error() );
                }
                return grab::overlay::Geometry{ std::move( *path ) };
            }
            if( !value.is_object() )
            {
                return reject( scope,
                               pointer,
                               "path must be an array of commands, or an object "
                               "carrying 'commands' and 'closed'" );
            }

            const Scope       inner        = nested( scope, pointer );
            const std::string commandsName = json_key( fieldCommands );
            if( !value.contains( commandsName ) )
            {
                return reject( inner,
                               child_pointer( pointer, fieldCommands ),
                               "missing required field" );
            }
            auto path = read_path_commands( inner,
                                            child_pointer( pointer, fieldCommands ),
                                            value.at( commandsName ) );
            if( !path.has_value() )
            {
                return std::unexpected( path.error() );
            }

            const std::string closedName = json_key( fieldClosed );
            if( value.contains( closedName ) )
            {
                const Json& closed = value.at( closedName );
                if( !closed.is_boolean() )
                {
                    return reject( inner,
                                   child_pointer( pointer, fieldClosed ),
                                   "must be a boolean" );
                }
                path->closed = closed.get<bool>();
            }
            return grab::overlay::Geometry{ std::move( *path ) };
        }

        // Exactly one geometry key. Two is ambiguous and none is not a shape;
        // both messages name what was actually found, because "invalid shape"
        // helps neither the author nor the model that wrote it.
        [[nodiscard]]
        grab::Result<grab::overlay::Geometry>
        read_geometry( const Scope&       scope,
                       const std::string& pointer,
                       const Json&        shape )
        {
            std::string found;
            std::size_t present = 0U;
            for( const auto key : geometryKeys )
            {
                if( !shape.contains( json_key( key ) ) )
                {
                    continue;
                }
                if( present > 0U )
                {
                    found.append( ", " );
                }
                found.append( key );
                ++present;
            }
            if( present != 1U )
            {
                std::string reason{ "shape needs exactly one geometry key of rect, "
                                    "ellipse, polygon or path; found " };
                reason.append( present == 0U ? std::string{ "none" } : found );
                return reject( scope, pointer, reason );
            }

            if( shape.contains( json_key( fieldRect ) ) )
            {
                return read_rect( scope,
                                  child_pointer( pointer, fieldRect ),
                                  shape.at( json_key( fieldRect ) ) );
            }
            if( shape.contains( json_key( fieldEllipse ) ) )
            {
                return read_ellipse( scope,
                                     child_pointer( pointer, fieldEllipse ),
                                     shape.at( json_key( fieldEllipse ) ) );
            }
            if( shape.contains( json_key( fieldPolygon ) ) )
            {
                return read_polygon( scope,
                                     child_pointer( pointer, fieldPolygon ),
                                     shape.at( json_key( fieldPolygon ) ) );
            }
            return read_path( scope,
                              child_pointer( pointer, fieldPath ),
                              shape.at( json_key( fieldPath ) ) );
        }

        [[nodiscard]]
        grab::Result<grab::overlay::StrokeStyle>
        read_stroke( const Scope&       scope,
                     const std::string& pointer,
                     const Json&        value )
        {
            if( !value.is_object() )
            {
                return reject( scope,
                               pointer,
                               "stroke must be an object with a color and an "
                               "optional width" );
            }
            const Scope                      inner = nested( scope, pointer );
            const grab::overlay::StrokeStyle defaults{};

            const auto color = require_color( inner, value, fieldColor );
            if( !color.has_value() )
            {
                return std::unexpected( color.error() );
            }
            const auto width =
                optional_number( inner,
                                 value,
                                 fieldWidth,
                                 static_cast<double>( defaults.width_px ) );
            if( !width.has_value() )
            {
                return std::unexpected( width.error() );
            }
            if( *width < 0.0 )
            {
                return reject( inner,
                               child_pointer( pointer, fieldWidth ),
                               "must not be negative" );
            }
            return grab::overlay::StrokeStyle{
                .color    = *color,
                .width_px = static_cast<float>( *width ),
            };
        }

        [[nodiscard]]
        grab::Result<grab::overlay::FillStyle>
        read_fill( const Scope&       scope,
                   const std::string& pointer,
                   const Json&        value )
        {
            if( !value.is_object() )
            {
                return reject( scope, pointer, "fill must be an object with a color" );
            }
            const Scope inner = nested( scope, pointer );
            const auto  color = require_color( inner, value, fieldColor );
            if( !color.has_value() )
            {
                return std::unexpected( color.error() );
            }
            return grab::overlay::FillStyle{ .color = *color };
        }

        constexpr std::string_view lifetimeForms =
            R"(expected "persistent", { "ttl_ms": N } or { "fade_ms": N })";

        // ttl and fade expire a shape from the scene itself, so a later
        // overlay.remove on that handle may find nothing. That is a run-time
        // no-op, not a load error — the loader tracks handles structurally and
        // does not simulate the scene.
        [[nodiscard]]
        grab::Result<grab::overlay::LifetimePolicy>
        read_lifetime( const Scope&       scope,
                       const std::string& pointer,
                       const Json&        value )
        {
            if( value.is_string() )
            {
                const auto text = value.get<std::string>();
                if( text != persistentText )
                {
                    std::string reason{ "unknown lifetime '" };
                    reason.append( text );
                    reason.append( "'; " );
                    reason.append( lifetimeForms );
                    return reject( scope, pointer, reason );
                }
                return grab::overlay::LifetimePolicy{ grab::overlay::Persistent{} };
            }
            if( !value.is_object() )
            {
                std::string reason{ "must be a lifetime; " };
                reason.append( lifetimeForms );
                return reject( scope, pointer, reason );
            }

            const bool has_ttl  = value.contains( json_key( fieldTtlMs ) );
            const bool has_fade = value.contains( json_key( fieldFadeMs ) );
            if( has_ttl == has_fade )
            {
                std::string reason{ "needs exactly one of 'ttl_ms' and 'fade_ms'; " };
                reason.append( lifetimeForms );
                return reject( scope, pointer, reason );
            }

            const auto field    = has_ttl ? fieldTtlMs : fieldFadeMs;
            const auto value_ms = bounded_integer( scope,
                                                   child_pointer( pointer, field ),
                                                   value.at( json_key( field ) ),
                                                   zeroDuration,
                                                   maximumDurationMs );
            if( !value_ms.has_value() )
            {
                return std::unexpected( value_ms.error() );
            }
            const std::chrono::milliseconds duration{ *value_ms };
            if( has_ttl )
            {
                return grab::overlay::LifetimePolicy{
                    grab::overlay::Ttl{ .duration = duration }
                };
            }
            return grab::overlay::LifetimePolicy{
                grab::overlay::Fade{ .duration = duration }
            };
        }

        // Every channel carries an easing and a duration. The duration is
        // REQUIRED: §4's governing rule is that no duration defaults to zero,
        // and a zero-duration channel is an animation that never runs.
        [[nodiscard]]
        grab::Result<grab::overlay::Channel>
        read_channel( const Scope& scope,
                      const Json&  node )
        {
            grab::overlay::Channel channel{};
            const auto             easing =
                optional_enum( scope,
                               node,
                               fieldEasing,
                               std::span<const EasingName>{ easingNames },
                               channel.easing,
                               "easing",
                               easingForms );
            if( !easing.has_value() )
            {
                return std::unexpected( easing.error() );
            }
            channel.easing      = *easing;

            const auto duration = require_integer( scope,
                                                   node,
                                                   fieldDurationMs,
                                                   zeroDuration,
                                                   maximumDurationMs );
            if( !duration.has_value() )
            {
                return std::unexpected( duration.error() );
            }
            channel.duration = std::chrono::milliseconds{ *duration };
            return channel;
        }

        [[nodiscard]]
        grab::Result<grab::overlay::AnimationSpec>
        read_animation( const Scope&       scope,
                        const std::string& pointer,
                        const Json&        value )
        {
            if( !value.is_object() )
            {
                return reject( scope,
                               pointer,
                               "animation must be an object of scale, opacity, "
                               "translate and reveal channels" );
            }
            grab::overlay::AnimationSpec spec{};

            const std::string            scaleName = json_key( fieldScale );
            if( value.contains( scaleName ) )
            {
                const std::string channelPointer = child_pointer( pointer, fieldScale );
                const Json&       node           = value.at( scaleName );
                if( !node.is_object() )
                {
                    return reject( scope, channelPointer, "must be an object" );
                }
                const Scope inner = nested( scope, channelPointer );
                const auto  base  = read_channel( inner, node );
                if( !base.has_value() )
                {
                    return std::unexpected( base.error() );
                }
                grab::overlay::ScaleChannel channel{};
                channel.easing   = base->easing;
                channel.duration = base->duration;

                const auto from =
                    optional_number( inner, node, fieldFrom, channel.from );
                if( !from.has_value() )
                {
                    return std::unexpected( from.error() );
                }
                const auto to = optional_number( inner, node, fieldTo, channel.to );
                if( !to.has_value() )
                {
                    return std::unexpected( to.error() );
                }
                channel.from = *from;
                channel.to   = *to;
                spec.scale   = channel;
            }

            const std::string opacityName = json_key( fieldOpacity );
            if( value.contains( opacityName ) )
            {
                const std::string channelPointer =
                    child_pointer( pointer, fieldOpacity );
                const Json& node = value.at( opacityName );
                if( !node.is_object() )
                {
                    return reject( scope, channelPointer, "must be an object" );
                }
                const Scope inner = nested( scope, channelPointer );
                const auto  base  = read_channel( inner, node );
                if( !base.has_value() )
                {
                    return std::unexpected( base.error() );
                }
                grab::overlay::OpacityChannel channel{};
                channel.easing   = base->easing;
                channel.duration = base->duration;

                const auto from =
                    optional_number( inner, node, fieldFrom, channel.from );
                if( !from.has_value() )
                {
                    return std::unexpected( from.error() );
                }
                const auto to = optional_number( inner, node, fieldTo, channel.to );
                if( !to.has_value() )
                {
                    return std::unexpected( to.error() );
                }
                channel.from = *from;
                channel.to   = *to;
                spec.opacity = channel;
            }

            const std::string translateName = json_key( fieldTranslate );
            if( value.contains( translateName ) )
            {
                const std::string channelPointer =
                    child_pointer( pointer, fieldTranslate );
                const Json& node = value.at( translateName );
                if( !node.is_object() )
                {
                    return reject( scope, channelPointer, "must be an object" );
                }
                const Scope inner = nested( scope, channelPointer );
                const auto  base  = read_channel( inner, node );
                if( !base.has_value() )
                {
                    return std::unexpected( base.error() );
                }
                grab::overlay::TranslateChannel channel{};
                channel.easing   = base->easing;
                channel.duration = base->duration;

                const auto dx    = optional_number( inner, node, fieldDx, channel.dx );
                if( !dx.has_value() )
                {
                    return std::unexpected( dx.error() );
                }
                const auto dy = optional_number( inner, node, fieldDy, channel.dy );
                if( !dy.has_value() )
                {
                    return std::unexpected( dy.error() );
                }
                channel.dx     = *dx;
                channel.dy     = *dy;
                spec.translate = channel;
            }

            const std::string revealName = json_key( fieldReveal );
            if( value.contains( revealName ) )
            {
                const std::string channelPointer = child_pointer( pointer, fieldReveal );
                const Json&       node           = value.at( revealName );
                if( !node.is_object() )
                {
                    return reject( scope, channelPointer, "must be an object" );
                }
                const Scope inner = nested( scope, channelPointer );
                const auto  base  = read_channel( inner, node );
                if( !base.has_value() )
                {
                    return std::unexpected( base.error() );
                }
                grab::overlay::RevealChannel channel{};
                channel.easing   = base->easing;
                channel.duration = base->duration;

                const auto axis  = optional_enum( inner,
                                                  node,
                                                  fieldAxis,
                                                  std::span<const AxisName>{ axisNames },
                                                  channel.axis,
                                                  "axis",
                                                  axisForms );
                if( !axis.has_value() )
                {
                    return std::unexpected( axis.error() );
                }
                const auto edge = optional_enum( inner,
                                                 node,
                                                 fieldFromEdge,
                                                 std::span<const EdgeName>{ edgeNames },
                                                 channel.from_edge,
                                                 "edge",
                                                 edgeForms );
                if( !edge.has_value() )
                {
                    return std::unexpected( edge.error() );
                }
                const auto from =
                    optional_number( inner, node, fieldFrom, channel.from );
                if( !from.has_value() )
                {
                    return std::unexpected( from.error() );
                }
                const auto to = optional_number( inner, node, fieldTo, channel.to );
                if( !to.has_value() )
                {
                    return std::unexpected( to.error() );
                }
                channel.axis      = *axis;
                channel.from_edge = *edge;
                channel.from      = *from;
                channel.to        = *to;
                spec.reveal       = channel;
            }
            return spec;
        }

        [[nodiscard]]
        grab::Result<grab::overlay::Shape>
        read_shape( const Scope&       scope,
                    const std::string& pointer,
                    const Json&        value )
        {
            if( !value.is_object() )
            {
                return reject( scope, pointer, "shape must be an object" );
            }
            const Scope inner    = nested( scope, pointer );

            auto        geometry = read_geometry( inner, pointer, value );
            if( !geometry.has_value() )
            {
                return std::unexpected( geometry.error() );
            }

            grab::overlay::Shape shape{};
            shape.geometry = std::move( *geometry );

            // A shape with neither stroke nor fill is LEGAL and invisible,
            // which is a useful thing to be able to say.
            const std::string strokeName = json_key( fieldStroke );
            if( value.contains( strokeName ) )
            {
                const auto stroke = read_stroke( inner,
                                                 child_pointer( pointer, fieldStroke ),
                                                 value.at( strokeName ) );
                if( !stroke.has_value() )
                {
                    return std::unexpected( stroke.error() );
                }
                shape.stroke = *stroke;
            }

            const std::string fillName = json_key( fieldFill );
            if( value.contains( fillName ) )
            {
                const auto fill = read_fill( inner,
                                             child_pointer( pointer, fieldFill ),
                                             value.at( fillName ) );
                if( !fill.has_value() )
                {
                    return std::unexpected( fill.error() );
                }
                shape.fill = *fill;
            }

            const std::string lifetimeName = json_key( fieldLifetime );
            if( value.contains( lifetimeName ) )
            {
                auto lifetime = read_lifetime( inner,
                                               child_pointer( pointer, fieldLifetime ),
                                               value.at( lifetimeName ) );
                if( !lifetime.has_value() )
                {
                    return std::unexpected( lifetime.error() );
                }
                shape.lifetime = std::move( *lifetime );
            }

            const auto band = optional_enum( inner,
                                             value,
                                             fieldBand,
                                             std::span<const BandName>{ bandNames },
                                             shape.band,
                                             "band",
                                             bandForms );
            if( !band.has_value() )
            {
                return std::unexpected( band.error() );
            }
            shape.band   = *band;

            const auto z = optional_integer( inner,
                                             value,
                                             fieldZ,
                                             defaultZIndex,
                                             zIndexMinimum,
                                             zIndexMaximum );
            if( !z.has_value() )
            {
                return std::unexpected( z.error() );
            }
            shape.z                         = static_cast<std::int32_t>( *z );

            const std::string animationName = json_key( fieldAnimation );
            if( value.contains( animationName ) )
            {
                const auto animation =
                    read_animation( inner,
                                    child_pointer( pointer, fieldAnimation ),
                                    value.at( animationName ) );
                if( !animation.has_value() )
                {
                    return std::unexpected( animation.error() );
                }
                shape.animation = *animation;
            }
            return shape;
        }

        [[nodiscard]]
        grab::Result<grab::overlay::Shape>
        require_shape( const Scope& scope,
                       const Json&  node )
        {
            const std::string name    = json_key( fieldShape );
            const std::string pointer = child_pointer( scope.pointer, fieldShape );
            if( !node.contains( name ) )
            {
                return reject( scope, pointer, "missing required field" );
            }
            return read_shape( scope, pointer, node.at( name ) );
        }

        [[nodiscard]]
        grab::Result<std::string>
        require_handle( const Scope& scope,
                        const Json&  node )
        {
            auto text = require_string( scope, node, fieldHandle );
            if( !text.has_value() )
            {
                return std::unexpected( text.error() );
            }
            if( text->empty() )
            {
                return reject( scope,
                               child_pointer( scope.pointer, fieldHandle ),
                               "must not be empty" );
            }
            return std::move( *text );
        }

        // An overlay.add with NO handle is fire-and-forget: drawable, never
        // referenced again. An empty one is a typo.
        [[nodiscard]]
        grab::Result<std::string>
        optional_handle( const Scope& scope,
                         const Json&  node )
        {
            if( !node.contains( json_key( fieldHandle ) ) )
            {
                return std::string{};
            }
            return require_handle( scope, node );
        }

        // ── Payloads ─────────────────────────────────────────

        [[nodiscard]]
        grab::Result<grab::sequence::Command>
        parse_capture( const Scope& scope,
                       const Json&  node )
        {
            const std::string outName     = json_key( fieldOut );
            const std::string locatorName = json_key( fieldLocator );
            const bool        has_out     = node.contains( outName );
            const bool        has_locator = node.contains( locatorName );
            if( has_out == has_locator )
            {
                return reject(
                    scope,
                    scope.pointer,
                    "screen.capture needs exactly one of 'out' and 'locator'"
                );
            }

            grab::sequence::CaptureCommand command{ .output = {}, .locator = {} };
            if( has_out )
            {
                auto output = require_string( scope, node, fieldOut );
                if( !output.has_value() )
                {
                    return std::unexpected( output.error() );
                }
                command.output = std::move( *output );
            }
            else
            {
                auto locator = require_string( scope, node, fieldLocator );
                if( !locator.has_value() )
                {
                    return std::unexpected( locator.error() );
                }
                command.locator = std::move( *locator );
            }
            return grab::sequence::Command{ std::move( command ) };
        }

        // time.wait is the ONE op whose declared duration is mandatory: every
        // other Timed op takes its dwell from defaulted options, so nothing
        // else can be silently zero. `ns` exists so a duration that is not a
        // whole millisecond still round-trips.
        [[nodiscard]]
        grab::Result<grab::sequence::Command>
        parse_wait( const Scope& scope,
                    const Json&  node )
        {
            const std::string msName = json_key( fieldMs );
            const std::string nsName = json_key( fieldNs );
            const bool        has_ms = node.contains( msName );
            const bool        has_ns = node.contains( nsName );
            if( has_ms == has_ns )
            {
                return reject( scope,
                               scope.pointer,
                               "time.wait needs exactly one of 'ms' and 'ns'" );
            }

            if( has_ms )
            {
                const auto value =
                    bounded_integer( scope,
                                     child_pointer( scope.pointer, fieldMs ),
                                     node.at( msName ),
                                     zeroDuration,
                                     maximumDurationMs );
                if( !value.has_value() )
                {
                    return std::unexpected( value.error() );
                }
                return grab::sequence::Command{
                    grab::sequence::WaitCommand{
                                                .duration = std::chrono::nanoseconds{
                            std::chrono::milliseconds{ *value }
                        }, }
                };
            }

            const auto value = bounded_integer( scope,
                                                child_pointer( scope.pointer, fieldNs ),
                                                node.at( nsName ),
                                                zeroDuration,
                                                maximumDurationNs );
            if( !value.has_value() )
            {
                return std::unexpected( value.error() );
            }
            return grab::sequence::Command{
                grab::sequence::WaitCommand{
                                            .duration = std::chrono::nanoseconds{ *value },
                                            }
            };
        }

        [[nodiscard]]
        grab::Result<grab::sequence::Command>
        parse_payload( const Scope&      scope,
                       const Json&       node,
                       grab::CommandKind kind )
        {
            switch( kind )
            {
                case grab::CommandKind::Type :
                    {
                        auto text = require_string( scope, node, fieldText );
                        if( !text.has_value() )
                        {
                            return std::unexpected( text.error() );
                        }
                        return grab::sequence::Command{
                            grab::sequence::TypeCommand{
                                                        .text = std::move( *text ),
                                                        }
                        };
                    }
                case grab::CommandKind::Key :
                    {
                        auto text = require_string( scope, node, fieldKey );
                        if( !text.has_value() )
                        {
                            return std::unexpected( text.error() );
                        }
                        return grab::sequence::Command{
                            grab::sequence::KeyCommand{
                                                       .key = std::move( *text ),
                                                       }
                        };
                    }
                case grab::CommandKind::KeyDown :
                    {
                        auto text = require_string( scope, node, fieldKey );
                        if( !text.has_value() )
                        {
                            return std::unexpected( text.error() );
                        }
                        return grab::sequence::Command{
                            grab::sequence::KeyDownCommand{
                                                           .key = std::move( *text ),
                                                           }
                        };
                    }
                case grab::CommandKind::KeyUp :
                    {
                        auto text = require_string( scope, node, fieldKey );
                        if( !text.has_value() )
                        {
                            return std::unexpected( text.error() );
                        }
                        return grab::sequence::Command{
                            grab::sequence::KeyUpCommand{
                                                         .key = std::move( *text ),
                                                         }
                        };
                    }
                case grab::CommandKind::Click :
                    {
                        const auto button = require_button( scope, node );
                        if( !button.has_value() )
                        {
                            return std::unexpected( button.error() );
                        }
                        return grab::sequence::Command{
                            grab::sequence::ClickCommand{
                                                         .button = *button,
                                                         }
                        };
                    }
                case grab::CommandKind::ClickAt :
                    {
                        const auto at = require_point( scope, node, fieldAt );
                        if( !at.has_value() )
                        {
                            return std::unexpected( at.error() );
                        }
                        const auto button = require_button( scope, node );
                        if( !button.has_value() )
                        {
                            return std::unexpected( button.error() );
                        }
                        return grab::sequence::Command{
                            grab::sequence::ClickAtCommand{
                                                           .at     = *at,
                                                           .button = *button,
                                                           }
                        };
                    }
                case grab::CommandKind::Press :
                    {
                        const auto button = require_button( scope, node );
                        if( !button.has_value() )
                        {
                            return std::unexpected( button.error() );
                        }
                        return grab::sequence::Command{
                            grab::sequence::PressCommand{
                                                         .button = *button,
                                                         }
                        };
                    }
                case grab::CommandKind::Release :
                    {
                        const auto button = require_button( scope, node );
                        if( !button.has_value() )
                        {
                            return std::unexpected( button.error() );
                        }
                        return grab::sequence::Command{
                            grab::sequence::ReleaseCommand{
                                                           .button = *button,
                                                           }
                        };
                    }
                case grab::CommandKind::Scroll :
                    {
                        const auto dx = optional_integer( scope,
                                                          node,
                                                          fieldDx,
                                                          0,
                                                          coordinateMinimum,
                                                          coordinateMaximum );
                        if( !dx.has_value() )
                        {
                            return std::unexpected( dx.error() );
                        }
                        const auto dy = optional_integer( scope,
                                                          node,
                                                          fieldDy,
                                                          0,
                                                          coordinateMinimum,
                                                          coordinateMaximum );
                        if( !dy.has_value() )
                        {
                            return std::unexpected( dy.error() );
                        }
                        return grab::sequence::Command{
                            grab::sequence::ScrollCommand{
                                                          .dx = static_cast<std::int32_t>( *dx ),
                                                          .dy = static_cast<std::int32_t>( *dy ),
                                                          }
                        };
                    }
                case grab::CommandKind::Warp :
                    {
                        const auto to = require_point( scope, node, fieldTo );
                        if( !to.has_value() )
                        {
                            return std::unexpected( to.error() );
                        }
                        return grab::sequence::Command{
                            grab::sequence::WarpCommand{
                                                        .to = *to,
                                                        }
                        };
                    }
                case grab::CommandKind::Move :
                    {
                        const auto from = optional_point( scope, node, fieldFrom );
                        if( !from.has_value() )
                        {
                            return std::unexpected( from.error() );
                        }
                        const auto to = require_point( scope, node, fieldTo );
                        if( !to.has_value() )
                        {
                            return std::unexpected( to.error() );
                        }
                        const auto options = require_options( scope, node );
                        if( !options.has_value() )
                        {
                            return std::unexpected( options.error() );
                        }
                        return grab::sequence::Command{
                            grab::sequence::MoveCommand{
                                                        .from    = *from,
                                                        .to      = *to,
                                                        .options = *options,
                                                        }
                        };
                    }
                case grab::CommandKind::Follow :
                    {
                        auto curve = require_curve( scope, node );
                        if( !curve.has_value() )
                        {
                            return std::unexpected( curve.error() );
                        }
                        const auto options = require_options( scope, node );
                        if( !options.has_value() )
                        {
                            return std::unexpected( options.error() );
                        }
                        return grab::sequence::Command{
                            grab::sequence::FollowCommand{
                                                          .path    = std::move( *curve ),
                                                          .options = *options,
                                                          }
                        };
                    }
                case grab::CommandKind::Drag :
                    {
                        const auto from = require_point( scope, node, fieldFrom );
                        if( !from.has_value() )
                        {
                            return std::unexpected( from.error() );
                        }
                        const auto to = require_point( scope, node, fieldTo );
                        if( !to.has_value() )
                        {
                            return std::unexpected( to.error() );
                        }
                        const auto button = require_button( scope, node );
                        if( !button.has_value() )
                        {
                            return std::unexpected( button.error() );
                        }
                        const auto options = require_options( scope, node );
                        if( !options.has_value() )
                        {
                            return std::unexpected( options.error() );
                        }
                        return grab::sequence::Command{
                            grab::sequence::DragCommand{
                                                        .from    = *from,
                                                        .to      = *to,
                                                        .button  = *button,
                                                        .options = *options,
                                                        }
                        };
                    }
                case grab::CommandKind::Capture :
                    return parse_capture( scope, node );
                case grab::CommandKind::Wait :
                    return parse_wait( scope, node );

                case grab::CommandKind::OverlayAdd :
                    {
                        auto handle = optional_handle( scope, node );
                        if( !handle.has_value() )
                        {
                            return std::unexpected( handle.error() );
                        }
                        auto shape = require_shape( scope, node );
                        if( !shape.has_value() )
                        {
                            return std::unexpected( shape.error() );
                        }
                        return grab::sequence::Command{
                            grab::sequence::OverlayAddCommand{
                                                              .handle = std::move( *handle ),
                                                              .shape  = std::move( *shape ),
                                                              }
                        };
                    }
                case grab::CommandKind::OverlayUpdate :
                    {
                        auto handle = require_handle( scope, node );
                        if( !handle.has_value() )
                        {
                            return std::unexpected( handle.error() );
                        }
                        auto shape = require_shape( scope, node );
                        if( !shape.has_value() )
                        {
                            return std::unexpected( shape.error() );
                        }
                        return grab::sequence::Command{
                            grab::sequence::OverlayUpdateCommand{
                                                                 .handle = std::move( *handle ),
                                                                 .shape  = std::move( *shape ),
                                                                 }
                        };
                    }
                case grab::CommandKind::OverlayRemove :
                    {
                        auto handle = require_handle( scope, node );
                        if( !handle.has_value() )
                        {
                            return std::unexpected( handle.error() );
                        }
                        return grab::sequence::Command{
                            grab::sequence::OverlayRemoveCommand{
                                                                 .handle = std::move( *handle ),
                                                                 }
                        };
                    }
                case grab::CommandKind::OverlayClear :
                    return grab::sequence::Command{
                        grab::sequence::OverlayClearCommand{}
                    };
                case grab::CommandKind::OverlayGrab :
                    return grab::sequence::Command{
                        grab::sequence::OverlayGrabCommand{}
                    };
                case grab::CommandKind::OverlayRelease :
                    return grab::sequence::Command{
                        grab::sequence::OverlayReleaseCommand{}
                    };
                case grab::CommandKind::OverlayAttach :
                    {
                        auto handle = require_handle( scope, node );
                        if( !handle.has_value() )
                        {
                            return std::unexpected( handle.error() );
                        }
                        // An absent offset means "keep the gap the shape
                        // already has", which is only knowable at run time —
                        // so a square picked up by its corner stays held by
                        // that corner.
                        const auto offset = optional_point( scope, node, fieldOffset );
                        if( !offset.has_value() )
                        {
                            return std::unexpected( offset.error() );
                        }
                        return grab::sequence::Command{
                            grab::sequence::OverlayAttachCommand{
                                                                 .handle = std::move( *handle ),
                                                                 .offset = *offset,
                                                                 }
                        };
                    }
                case grab::CommandKind::OverlayDetach :
                    {
                        auto handle = require_handle( scope, node );
                        if( !handle.has_value() )
                        {
                            return std::unexpected( handle.error() );
                        }
                        return grab::sequence::Command{
                            grab::sequence::OverlayDetachCommand{
                                                                 .handle = std::move( *handle ),
                                                                 }
                        };
                    }

                // The other half of the descriptor table. is_sequence_command()
                // has already rejected these by name, so reaching one here
                // would mean the two lists had drifted apart.
                case grab::CommandKind::Doctor :
                case grab::CommandKind::Daemon :
                case grab::CommandKind::DragCurve :
                case grab::CommandKind::Windows :
                case grab::CommandKind::Focus :
                case grab::CommandKind::Place :
                case grab::CommandKind::Batch :
                case grab::CommandKind::Compare :
                case grab::CommandKind::Watch :
                case grab::CommandKind::Session :
                case grab::CommandKind::OverlayTrail :
                case grab::CommandKind::OverlayShape :
                case grab::CommandKind::OverlayFeedback :
                case grab::CommandKind::OverlaySketch :
                case grab::CommandKind::Play :
                case grab::CommandKind::Count :
                    break;
            }
            return grab::fail( grab::ErrorCode::InternalFault,
                               "no sequence payload parser for this command kind" );
        }

        // ── after, on_error ──────────────────────────────────

        using LabelMap = std::unordered_map<std::string, std::size_t>;

        [[nodiscard]]
        grab::Result<std::vector<grab::sequence::StepId>>
        parse_after( const Scope&    scope,
                     const Json&     node,
                     std::size_t     index,
                     const LabelMap& labels,
                     std::size_t     step_count )
        {
            std::vector<grab::sequence::StepId> after;

            const std::string                   name = json_key( fieldAfter );
            const std::string pointer = child_pointer( scope.pointer, fieldAfter );
            if( !node.contains( name ) )
            {
                // A step with no `after` depends on the PRECEDING step in
                // document order, and the first step depends on nothing. That
                // is what makes a plain list read top-to-bottom exactly like
                // the bash script it replaces.
                if( index > 0U )
                {
                    after.push_back( step_id_at( index - 1U ) );
                }
                return after;
            }

            const Json& value = node.at( name );
            if( !value.is_array() )
            {
                return reject( scope, pointer, "'after' must be an array" );
            }

            after.reserve( value.size() );
            for( std::size_t slot = 0U; slot < value.size(); ++slot )
            {
                const std::string entryPointer = element_pointer( pointer, slot );
                const Json&       entry        = value.at( slot );
                std::size_t       target       = 0U;

                if( entry.is_string() )
                {
                    const auto label = entry.get<std::string>();
                    const auto found = labels.find( label );
                    if( found == labels.end() )
                    {
                        std::string reason{ "depends on '" };
                        reason.append( label );
                        reason.append( "', but no step carries that label" );
                        return reject( scope, entryPointer, reason );
                    }
                    target = found->second;
                }
                else if( entry.is_number_integer() )
                {
                    const auto position =
                        bounded_integer( scope,
                                         entryPointer,
                                         entry,
                                         0,
                                         static_cast<std::int64_t>( step_count ) - 1 );
                    if( !position.has_value() )
                    {
                        std::string reason{ "depends on a step index outside the "
                                            "document, which holds " };
                        reason.append( std::to_string( step_count ) );
                        reason.append( " steps" );
                        return reject( scope, entryPointer, reason );
                    }
                    target = static_cast<std::size_t>( *position );
                }
                else
                {
                    return reject( scope,
                                   entryPointer,
                                   "each 'after' entry must be a step label or a "
                                   "document index" );
                }

                if( target == index )
                {
                    // AdjacencyGraph::add_edge drops a self-loop and returns
                    // false, so the edge never enters the graph and the
                    // topological sort cannot see it. Rejecting it is the
                    // loader's job, here.
                    return reject( scope, entryPointer, "depends on itself" );
                }

                const auto id = step_id_at( target );
                if( std::ranges::find( after, id ) != after.end() )
                {
                    std::string reason{ "lists step index " };
                    reason.append( std::to_string( target ) );
                    reason.append( " as a dependency twice" );
                    return reject( scope, entryPointer, reason );
                }
                after.push_back( id );
            }
            return after;
        }

        // ── Overlay handles ──────────────────────────────────

        using HandleSet = std::unordered_set<std::string>;

        // A handle is a document-level name, so the loader checks it the way
        // it checks `after`: a handle used before its overlay.add, or added
        // again while still live, cannot resolve to a ShapeId at run time.
        // That is structural, not policy — exactly like a dangling `after` —
        // so it is rejected here rather than left to fail silently mid-run.
        //
        // Liveness is tracked in DOCUMENT ORDER, which is also the order a
        // reader checks it in. It is deliberately not a scene simulation:
        // overlay.clear does not retire handles, and a ttl or fade shape that
        // expires on its own still counts as live, because the loader cannot
        // know when either happens and §3.2 already says a remove that finds
        // nothing succeeds.
        [[nodiscard]]
        grab::Result<void>
        track_handles( const Scope&                   scope,
                       const grab::sequence::Command& command,
                       HandleSet&                     live )
        {
            const std::string pointer = child_pointer( scope.pointer, fieldHandle );

            if( const auto* add =
                    std::get_if<grab::sequence::OverlayAddCommand>( &command ) )
            {
                if( add->handle.empty() )
                {
                    return {};
                }
                if( !live.insert( add->handle ).second )
                {
                    std::string reason{ "reuses handle '" };
                    reason.append( add->handle );
                    reason.append( "' while it is still live; remove it before "
                                   "adding it again" );
                    return reject( scope, pointer, reason );
                }
                return {};
            }

            const std::string* used    = nullptr;
            bool               retires = false;
            if( const auto* update =
                    std::get_if<grab::sequence::OverlayUpdateCommand>( &command ) )
            {
                used = &update->handle;
            }
            else if( const auto* remove =
                         std::get_if<grab::sequence::OverlayRemoveCommand>( &command ) )
            {
                used    = &remove->handle;
                retires = true;
            }
            else if( const auto* attach =
                         std::get_if<grab::sequence::OverlayAttachCommand>( &command ) )
            {
                used = &attach->handle;
            }
            else if( const auto* detach =
                         std::get_if<grab::sequence::OverlayDetachCommand>( &command ) )
            {
                used = &detach->handle;
            }
            if( used == nullptr )
            {
                return {};
            }

            if( !live.contains( *used ) )
            {
                std::string reason{ "uses handle '" };
                reason.append( *used );
                reason.append( "' before any overlay.add creates it" );
                return reject( scope, pointer, reason );
            }
            if( retires )
            {
                // Retiring it here is what makes reuse AFTER a remove legal:
                // the name is free again, and the next add is a new shape.
                live.erase( *used );
            }
            return {};
        }

        struct ErrorPolicySpec
        {
                grab::sequence::ErrorPolicy policy{ grab::sequence::ErrorPolicy::Abort };
                std::string                 target{};
        };

        [[nodiscard]]
        grab::Result<ErrorPolicySpec>
        parse_error_policy( const Scope&    scope,
                            const Json&     node,
                            const LabelMap& labels )
        {
            const std::string name    = json_key( fieldOnError );
            const std::string pointer = child_pointer( scope.pointer, fieldOnError );
            if( !node.contains( name ) )
            {
                return ErrorPolicySpec{
                    .policy = grab::sequence::ErrorPolicy::Abort,
                    .target = {},
                };
            }

            const auto text = require_string( scope, node, fieldOnError );
            if( !text.has_value() )
            {
                return std::unexpected( text.error() );
            }

            if( text->starts_with( gotoPrefix ) )
            {
                std::string target = text->substr( gotoPrefix.size() );
                if( target.empty() )
                {
                    return reject( scope, pointer, "'goto:' needs a target step label" );
                }
                if( !labels.contains( target ) )
                {
                    std::string reason{ "jumps to '" };
                    reason.append( target );
                    reason.append( "', but no step carries that label" );
                    return reject( scope, pointer, reason );
                }
                return ErrorPolicySpec{
                    .policy = grab::sequence::ErrorPolicy::Goto,
                    .target = std::move( target ),
                };
            }

            const auto policy = grab::sequence::error_policy_from_name( *text );
            if( !policy.has_value() || *policy == grab::sequence::ErrorPolicy::Goto )
            {
                std::string reason{ "unknown error policy '" };
                reason.append( *text );
                reason.append( "'; expected abort, continue or goto:<label>" );
                return reject( scope, pointer, reason );
            }
            return ErrorPolicySpec{
                .policy = *policy,
                .target = {},
            };
        }

        [[nodiscard]]
        grab::Result<grab::sequence::PacingOptions>
        parse_pacing( const Scope& scope,
                      const Json&  document )
        {
            grab::sequence::PacingOptions options{
                .mode  = grab::sequence::PacingMode::Strict,
                .grace = std::chrono::milliseconds::zero(),
            };

            const std::string name    = json_key( fieldPacing );
            const std::string pointer = child_pointer( scope.pointer, fieldPacing );
            if( !document.contains( name ) )
            {
                return options;
            }
            const Json& value = document.at( name );
            if( !value.is_object() )
            {
                return reject( scope, pointer, "must be an object" );
            }

            const Scope inner{
                .origin  = scope.origin,
                .pointer = pointer,
                .subject = scope.subject,
            };

            if( value.contains( json_key( fieldMode ) ) )
            {
                const auto text = require_string( inner, value, fieldMode );
                if( !text.has_value() )
                {
                    return std::unexpected( text.error() );
                }
                const auto mode = grab::sequence::pacing_mode_from_name( *text );
                if( !mode.has_value() )
                {
                    std::string reason{ "unknown pacing mode '" };
                    reason.append( *text );
                    reason.append( "'; expected strict, grace or precise" );
                    return reject( inner, child_pointer( pointer, fieldMode ), reason );
                }
                options.mode = *mode;
            }

            const auto grace = optional_integer( inner,
                                                 value,
                                                 fieldGraceMs,
                                                 zeroDuration,
                                                 zeroDuration,
                                                 maximumDurationMs );
            if( !grace.has_value() )
            {
                return std::unexpected( grace.error() );
            }
            options.grace = std::chrono::milliseconds{ *grace };
            return options;
        }

        // ── Serialization ────────────────────────────────────

        [[nodiscard]]
        bool
        is_default_options( const grab::input::DragOptions& options ) noexcept
        {
            const grab::input::DragOptions defaults{};
            return options.interpolation_steps ==
                   defaults.interpolation_steps &&
                   options.step_dwell ==
                   defaults.step_dwell &&
                   options.path == defaults.path;
        }

        [[nodiscard]]
        std::string_view
        path_name( grab::input::DragOptions::Path value ) noexcept
        {
            const auto* const found =
                std::ranges::find( pathNames, value, &PathName::value );
            return found == pathNames.end() ? pathNames.front().text : found->text;
        }

        void
        write_point( Json&                 node,
                     std::string_view      field,
                     grab::geometry::Point point )
        {
            Json written = Json::array();
            written.push_back( point.x );
            written.push_back( point.y );
            node[json_key( field )] = std::move( written );
        }

        void
        write_button( Json&        node,
                      std::uint8_t code )
        {
            for( const auto& entry : buttonNames )
            {
                if( entry.canonical && entry.code == code )
                {
                    node[json_key( fieldButton )] = std::string{ entry.text };
                    return;
                }
            }
            node[json_key( fieldButton )] = code;
        }

        void
        write_options( Json&                           node,
                       const grab::input::DragOptions& options )
        {
            if( is_default_options( options ) )
            {
                return;
            }
            Json written                          = Json::object();
            written[json_key( fieldSteps )]       = options.interpolation_steps;
            written[json_key( fieldStepDwellMs )] = options.step_dwell.count();
            written[json_key( fieldPath )] = std::string{ path_name( options.path ) };
            node[json_key( fieldOptions )] = std::move( written );
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&                              node,
                       const grab::sequence::TypeCommand& command )
        {
            node[json_key( fieldText )] = command.text;
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&                             node,
                       const grab::sequence::KeyCommand& command )
        {
            node[json_key( fieldKey )] = command.key;
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&                                 node,
                       const grab::sequence::KeyDownCommand& command )
        {
            node[json_key( fieldKey )] = command.key;
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&                               node,
                       const grab::sequence::KeyUpCommand& command )
        {
            node[json_key( fieldKey )] = command.key;
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&                               node,
                       const grab::sequence::ClickCommand& command )
        {
            write_button( node, command.button );
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&                                 node,
                       const grab::sequence::ClickAtCommand& command )
        {
            write_point( node, fieldAt, command.at );
            write_button( node, command.button );
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&                               node,
                       const grab::sequence::PressCommand& command )
        {
            write_button( node, command.button );
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&                                 node,
                       const grab::sequence::ReleaseCommand& command )
        {
            write_button( node, command.button );
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&                                node,
                       const grab::sequence::ScrollCommand& command )
        {
            node[json_key( fieldDx )] = command.dx;
            node[json_key( fieldDy )] = command.dy;
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&                              node,
                       const grab::sequence::WarpCommand& command )
        {
            write_point( node, fieldTo, command.to );
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&                              node,
                       const grab::sequence::MoveCommand& command )
        {
            if( command.from.has_value() )
            {
                write_point( node, fieldFrom, *command.from );
            }
            write_point( node, fieldTo, command.to );
            write_options( node, command.options );
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&                                node,
                       const grab::sequence::FollowCommand& command )
        {
            Json control = Json::array();
            for( const auto& point : command.path.control )
            {
                Json pair = Json::array();
                pair.push_back( point.x );
                pair.push_back( point.y );
                control.push_back( std::move( pair ) );
            }
            node[json_key( fieldCurve )] = std::move( control );
            write_options( node, command.options );
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&                              node,
                       const grab::sequence::DragCommand& command )
        {
            write_point( node, fieldFrom, command.from );
            write_point( node, fieldTo, command.to );
            write_button( node, command.button );
            write_options( node, command.options );
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&                                 node,
                       const grab::sequence::CaptureCommand& command )
        {
            if( command.output.empty() == command.locator.empty() )
            {
                return grab::fail(
                    grab::ErrorCode::InvalidArgument,
                    "a screen.capture step carries neither or both of "
                    "output and locator, which the grammar cannot spell"
                );
            }
            if( !command.output.empty() )
            {
                node[json_key( fieldOut )] = command.output;
            }
            else
            {
                node[json_key( fieldLocator )] = command.locator;
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&                              node,
                       const grab::sequence::WaitCommand& command )
        {
            const auto whole = std::chrono::duration_cast<std::chrono::milliseconds>(
                command.duration
            );
            if( std::chrono::nanoseconds{ whole } == command.duration )
            {
                node[json_key( fieldMs )] = whole.count();
                return {};
            }
            node[json_key( fieldNs )] = command.duration.count();
            return {};
        }

        // ── Overlay serialization ────────────────────────────
        //
        // Everything below omits what equals its default, so a round-tripped
        // document stays the one a human would have written: no stroke, no
        // fill, no lifetime, no band, no z and no animation unless the shape
        // actually carries them.

        [[nodiscard]]
        Json
        space_point_json( grab::SpacePoint point )
        {
            Json pair = Json::array();
            pair.push_back( point.x );
            pair.push_back( point.y );
            return pair;
        }

        // The write side of optional_enum. An enumerator with no entry cannot
        // happen — the tables are exhaustive — so the fallback is the table's
        // own first name rather than an error path nothing can reach.
        template<typename EntryT,
                 std::size_t Size>
        [[nodiscard]]
        std::string_view
        enum_text( const std::array<EntryT,
                                    Size>&   table,
                   decltype( EntryT::value ) value ) noexcept
        {
            const auto found = std::ranges::find( table, value, &EntryT::value );
            return found == table.end() ? table.front().text : found->text;
        }

        void
        write_geometry( Json&                          node,
                        const grab::overlay::Geometry& geometry )
        {
            if( const auto* rect = std::get_if<grab::overlay::Rect>( &geometry ) )
            {
                Json written                = Json::object();
                written[json_key( fieldX )] = rect->bounds.x;
                written[json_key( fieldY )] = rect->bounds.y;
                written[json_key( fieldW )] = rect->bounds.w;
                written[json_key( fieldH )] = rect->bounds.h;
                node[json_key( fieldRect )] = std::move( written );
                return;
            }
            if( const auto* ellipse = std::get_if<grab::overlay::Ellipse>( &geometry ) )
            {
                Json written                     = Json::object();
                written[json_key( fieldCenter )] = space_point_json( ellipse->center );
                // Equal radii write the shorthand back out, so the spelling a
                // document used is the spelling it keeps.
                if( ellipse->radius_x == ellipse->radius_y )
                {
                    written[json_key( fieldRadius )] = ellipse->radius_x;
                }
                else
                {
                    written[json_key( fieldRadiusX )] = ellipse->radius_x;
                    written[json_key( fieldRadiusY )] = ellipse->radius_y;
                }
                node[json_key( fieldEllipse )] = std::move( written );
                return;
            }
            if( const auto* polygon = std::get_if<grab::overlay::Polygon>( &geometry ) )
            {
                Json written = Json::array();
                for( const auto point : polygon->points )
                {
                    written.push_back( space_point_json( point ) );
                }
                node[json_key( fieldPolygon )] = std::move( written );
                return;
            }

            const auto& path     = std::get<grab::overlay::Path>( geometry );
            Json        commands = Json::array();
            for( const auto& command : path.commands )
            {
                if( const auto* move = std::get_if<grab::overlay::MoveTo>( &command ) )
                {
                    Json written                   = Json::object();
                    written[json_key( fieldMove )] = space_point_json( move->point );
                    commands.push_back( std::move( written ) );
                    continue;
                }
                if( const auto* line = std::get_if<grab::overlay::LineTo>( &command ) )
                {
                    Json written                   = Json::object();
                    written[json_key( fieldLine )] = space_point_json( line->point );
                    commands.push_back( std::move( written ) );
                    continue;
                }
                if( const auto* bezier =
                        std::get_if<grab::overlay::BezierTo>( &command ) )
                {
                    Json control = Json::array();
                    for( const auto point : bezier->control )
                    {
                        control.push_back( space_point_json( point ) );
                    }
                    Json written                     = Json::object();
                    written[json_key( fieldBezier )] = std::move( control );
                    commands.push_back( std::move( written ) );
                    continue;
                }
                commands.push_back( std::string{ closeCommandText } );
            }
            if( !path.closed )
            {
                node[json_key( fieldPath )] = std::move( commands );
                return;
            }
            // The object form only where the array cannot carry the answer.
            Json written                       = Json::object();
            written[json_key( fieldCommands )] = std::move( commands );
            written[json_key( fieldClosed )]   = path.closed;
            node[json_key( fieldPath )]        = std::move( written );
        }

        void
        write_animation( Json&                               node,
                         const grab::overlay::AnimationSpec& animation )
        {
            Json written = Json::object();
            if( animation.scale.has_value() )
            {
                Json channel = Json::object();
                channel[json_key( fieldEasing )] =
                    std::string{ enum_text( easingNames, animation.scale->easing ) };
                channel[json_key( fieldDurationMs )] = animation.scale->duration.count();
                channel[json_key( fieldFrom )]       = animation.scale->from;
                channel[json_key( fieldTo )]         = animation.scale->to;
                written[json_key( fieldScale )]      = std::move( channel );
            }
            if( animation.opacity.has_value() )
            {
                Json channel = Json::object();
                channel[json_key( fieldEasing )] =
                    std::string{ enum_text( easingNames, animation.opacity->easing ) };
                channel[json_key( fieldDurationMs )] =
                    animation.opacity->duration.count();
                channel[json_key( fieldFrom )]    = animation.opacity->from;
                channel[json_key( fieldTo )]      = animation.opacity->to;
                written[json_key( fieldOpacity )] = std::move( channel );
            }
            if( animation.translate.has_value() )
            {
                Json channel = Json::object();
                channel[json_key( fieldEasing )] =
                    std::string{ enum_text( easingNames, animation.translate->easing ) };
                channel[json_key( fieldDurationMs )] =
                    animation.translate->duration.count();
                channel[json_key( fieldDx )]        = animation.translate->dx;
                channel[json_key( fieldDy )]        = animation.translate->dy;
                written[json_key( fieldTranslate )] = std::move( channel );
            }
            if( animation.reveal.has_value() )
            {
                Json channel = Json::object();
                channel[json_key( fieldEasing )] =
                    std::string{ enum_text( easingNames, animation.reveal->easing ) };
                channel[json_key( fieldDurationMs )] =
                    animation.reveal->duration.count();
                channel[json_key( fieldAxis )] =
                    std::string{ enum_text( axisNames, animation.reveal->axis ) };
                channel[json_key( fieldFromEdge )] =
                    std::string{ enum_text( edgeNames, animation.reveal->from_edge ) };
                channel[json_key( fieldFrom )]   = animation.reveal->from;
                channel[json_key( fieldTo )]     = animation.reveal->to;
                written[json_key( fieldReveal )] = std::move( channel );
            }
            node[json_key( fieldAnimation )] = std::move( written );
        }

        void
        write_shape( Json&                       node,
                     const grab::overlay::Shape& shape )
        {
            Json written = Json::object();
            write_geometry( written, shape.geometry );

            if( shape.stroke.has_value() )
            {
                Json stroke                      = Json::object();
                stroke[json_key( fieldColor )]   = color_text( shape.stroke->color );
                stroke[json_key( fieldWidth )]   = shape.stroke->width_px;
                written[json_key( fieldStroke )] = std::move( stroke );
            }
            if( shape.fill.has_value() )
            {
                Json fill                      = Json::object();
                fill[json_key( fieldColor )]   = color_text( shape.fill->color );
                written[json_key( fieldFill )] = std::move( fill );
            }

            if( const auto* ttl = std::get_if<grab::overlay::Ttl>( &shape.lifetime ) )
            {
                Json lifetime                      = Json::object();
                lifetime[json_key( fieldTtlMs )]   = ttl->duration.count();
                written[json_key( fieldLifetime )] = std::move( lifetime );
            }
            else if( const auto* fade =
                         std::get_if<grab::overlay::Fade>( &shape.lifetime ) )
            {
                Json lifetime                      = Json::object();
                lifetime[json_key( fieldFadeMs )]  = fade->duration.count();
                written[json_key( fieldLifetime )] = std::move( lifetime );
            }

            if( shape.band != grab::overlay::Band::Annotation )
            {
                written[json_key( fieldBand )] =
                    std::string{ enum_text( bandNames, shape.band ) };
            }
            if( shape.z != defaultZIndex )
            {
                written[json_key( fieldZ )] = shape.z;
            }
            if( shape.animation.has_value() )
            {
                write_animation( written, *shape.animation );
            }
            node[json_key( fieldShape )] = std::move( written );
        }

        // A handle the grammar cannot spell is the overlay twin of a capture
        // with neither target: unreachable through parse(), and refused rather
        // than written as something it is not.
        [[nodiscard]]
        grab::Result<void>
        write_handle( Json&              node,
                      const std::string& handle,
                      std::string_view   op )
        {
            if( handle.empty() )
            {
                std::string reason{ "a " };
                reason.append( op );
                reason.append( " step carries no handle, which the grammar cannot "
                               "spell" );
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   std::move( reason ) );
            }
            node[json_key( fieldHandle )] = handle;
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&                                    node,
                       const grab::sequence::OverlayAddCommand& command )
        {
            // The only handle that may be absent: fire-and-forget, drawable,
            // never referenced again.
            if( !command.handle.empty() )
            {
                node[json_key( fieldHandle )] = command.handle;
            }
            write_shape( node, command.shape );
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&                                       node,
                       const grab::sequence::OverlayUpdateCommand& command )
        {
            const auto handle = write_handle( node, command.handle, "overlay.update" );
            if( !handle.has_value() )
            {
                return handle;
            }
            write_shape( node, command.shape );
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&                                       node,
                       const grab::sequence::OverlayRemoveCommand& command )
        {
            return write_handle( node, command.handle, "overlay.remove" );
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&,
                       const grab::sequence::OverlayClearCommand& )
        {
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&,
                       const grab::sequence::OverlayGrabCommand& )
        {
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&,
                       const grab::sequence::OverlayReleaseCommand& )
        {
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&                                       node,
                       const grab::sequence::OverlayAttachCommand& command )
        {
            const auto handle = write_handle( node, command.handle, "overlay.attach" );
            if( !handle.has_value() )
            {
                return handle;
            }
            if( command.offset.has_value() )
            {
                write_point( node, fieldOffset, *command.offset );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        write_payload( Json&                                       node,
                       const grab::sequence::OverlayDetachCommand& command )
        {
            return write_handle( node, command.handle, "overlay.detach" );
        }

        [[nodiscard]]
        grab::Result<void>
        write_after( Json&                                 node,
                     std::span<const grab::sequence::Step> steps,
                     std::size_t                           index,
                     const grab::sequence::Step&           step )
        {
            // The implicit edge is exactly "one predecessor, the step before
            // me". Omitting it here is what lets a plain document round-trip
            // to itself instead of growing an `after` on every line. An EMPTY
            // `after` on a non-first step is NOT implicit: it declares a root,
            // and dropping it would silently re-attach the step.
            const bool implicit = index == 0U
                                    ? step.after.empty()
                                    : step.after.size() ==
                                          1U &&
                                          step.after.front() == step_id_at( index - 1U );
            if( implicit )
            {
                return {};
            }

            Json written = Json::array();
            for( const auto predecessor : step.after )
            {
                const auto slot = static_cast<std::size_t>( predecessor.index() );
                if( slot >= steps.size() || steps[slot].id != predecessor )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "a step depends on an id the sequence does "
                                       "not contain" );
                }
                if( steps[slot].label.empty() )
                {
                    written.push_back( slot );
                }
                else
                {
                    written.push_back( steps[slot].label );
                }
            }
            node[json_key( fieldAfter )] = std::move( written );
            return {};
        }

        // ── The document ─────────────────────────────────────

        [[nodiscard]]
        grab::Result<Sequence>
        parse_document( std::string_view json,
                        std::string_view origin )
        {
            const Scope scope{
                .origin  = origin,
                .pointer = std::string{ documentBase },
                .subject = {},
            };

            Json document;
            try
            {
                document = Json::parse( json.begin(), json.end() );
            }
            catch( const Json::parse_error& error )
            {
                return reject( scope, rootPointer, error.what() );
            }

            if( !document.is_object() )
            {
                return reject( scope,
                               rootPointer,
                               "the document must be a JSON object" );
            }

            const std::string versionName = json_key( fieldSchemaVersion );
            if( document.contains( versionName ) )
            {
                const auto version =
                    bounded_integer( scope,
                                     child_pointer( documentBase, fieldSchemaVersion ),
                                     document.at( versionName ),
                                     supportedSchemaVersion,
                                     supportedSchemaVersion );
                if( !version.has_value() )
                {
                    return reject( scope,
                                   child_pointer( documentBase, fieldSchemaVersion ),
                                   "unsupported schema version; this build reads "
                                   "version 1" );
                }
            }

            std::string       name;
            const std::string sequenceName = json_key( fieldSequence );
            if( document.contains( sequenceName ) )
            {
                auto text = require_string( scope, document, fieldSequence );
                if( !text.has_value() )
                {
                    return std::unexpected( text.error() );
                }
                name = std::move( *text );
            }

            const auto pacing = parse_pacing( scope, document );
            if( !pacing.has_value() )
            {
                return std::unexpected( pacing.error() );
            }

            const std::string stepsName    = json_key( fieldSteps );
            const std::string stepsPointer = child_pointer( documentBase, fieldSteps );
            if( !document.contains( stepsName ) )
            {
                return reject( scope, stepsPointer, "missing required field" );
            }
            const Json& stepNodes = document.at( stepsName );
            if( !stepNodes.is_array() )
            {
                return reject( scope, stepsPointer, "must be an array" );
            }
            if( stepNodes.size() > grab::sequence::maxSteps )
            {
                std::string reason{ "holds " };
                reason.append( std::to_string( stepNodes.size() ) );
                reason.append( " steps; the maximum is " );
                reason.append( std::to_string( grab::sequence::maxSteps ) );
                return reject( scope, stepsPointer, reason );
            }

            // Pass one collects labels, so `after` may name a step further
            // down the document as long as the result is still acyclic.
            LabelMap                 labels;
            std::vector<std::string> stepLabels( stepNodes.size() );
            for( std::size_t index = 0U; index < stepNodes.size(); ++index )
            {
                const Json&       node    = stepNodes.at( index );
                const std::string pointer = step_pointer( index );
                if( !node.is_object() )
                {
                    return reject( scope, pointer, "must be an object" );
                }

                const std::string idName = json_key( fieldId );
                if( !node.contains( idName ) )
                {
                    continue;
                }
                const std::string idPointer = child_pointer( pointer, fieldId );
                const Json&       value     = node.at( idName );
                if( !value.is_string() )
                {
                    return reject( scope, idPointer, "must be a string" );
                }
                auto label = value.get<std::string>();
                if( label.empty() )
                {
                    return reject( scope, idPointer, "must not be empty" );
                }
                if( !labels.emplace( label, index ).second )
                {
                    // An ambiguous `after` target is unresolvable, not merely
                    // bad style.
                    std::string reason{ "duplicate step label '" };
                    reason.append( label );
                    reason.append( "'" );
                    return reject( scope, idPointer, reason );
                }
                stepLabels[index] = std::move( label );
            }

            std::vector<grab::sequence::Step> steps;
            steps.reserve( stepNodes.size() );

            // Overlay handles resolve in document order, not through the
            // label pre-pass: an overlay.add is what creates one, so naming a
            // handle further down the document is a use before its creation
            // rather than a forward reference.
            HandleSet liveHandles;
            for( std::size_t index = 0U; index < stepNodes.size(); ++index )
            {
                const Json& node = stepNodes.at( index );
                const Scope stepScope{
                    .origin  = origin,
                    .pointer = step_pointer( index ),
                    .subject = step_subject( stepLabels[index], index ),
                };

                const auto op = require_string( stepScope, node, fieldOp );
                if( !op.has_value() )
                {
                    return std::unexpected( op.error() );
                }
                const std::string opPointer =
                    child_pointer( stepScope.pointer, fieldOp );
                const auto kind = grab::command_kind( *op );
                if( !kind.has_value() )
                {
                    std::string reason{ "unknown op '" };
                    reason.append( *op );
                    reason.append( "'" );
                    return reject( stepScope, opPointer, reason );
                }
                if( !grab::sequence::is_sequence_command( *kind ) )
                {
                    // A DIFFERENT mistake from an unknown op: the name is real,
                    // the verb simply has no payload struct and therefore
                    // cannot be a step.
                    std::string reason{ "op '" };
                    reason.append( *op );
                    reason.append( "' is not available as a sequence step" );
                    return reject( stepScope, opPointer, reason );
                }

                auto command = parse_payload( stepScope, node, *kind );
                if( !command.has_value() )
                {
                    return std::unexpected( command.error() );
                }

                const auto handles = track_handles( stepScope, *command, liveHandles );
                if( !handles.has_value() )
                {
                    return std::unexpected( handles.error() );
                }

                auto after =
                    parse_after( stepScope, node, index, labels, stepNodes.size() );
                if( !after.has_value() )
                {
                    return std::unexpected( after.error() );
                }

                auto policy = parse_error_policy( stepScope, node, labels );
                if( !policy.has_value() )
                {
                    return std::unexpected( policy.error() );
                }

                // Present under every mode, honoured only under `precise`.
                // Rejecting it under `strict` would stop one document running
                // under all three modes, which is the point of having modes.
                const auto extra = optional_integer( stepScope,
                                                     node,
                                                     fieldExtraGraceMs,
                                                     zeroDuration,
                                                     zeroDuration,
                                                     maximumDurationMs );
                if( !extra.has_value() )
                {
                    return std::unexpected( extra.error() );
                }

                log::verbose(
                    [&stepScope, &op, index]( auto& event )
                    {
                        event.tag( log::tags::sequence )
                            .value( "step", index )
                            .value( "op", *op )
                            .value( "at", stepScope.pointer );
                    }
                );

                steps.push_back( grab::sequence::Step{
                    .id              = grab::sequence::StepId{},
                    .label           = stepLabels[index],
                    .command         = std::move( *command ),
                    .after           = std::move( *after ),
                    .on_error        = policy->policy,
                    .on_error_target = policy->target,
                    .extra_grace     = std::chrono::milliseconds{ *extra },
                } );
            }

            // Everything build() can still reject has been pre-checked with a
            // pointer above, except the cycle — which has no single pointer to
            // name, because it is a property of the graph rather than of one
            // entry.
            auto built =
                Sequence::build( std::move( steps ), *pacing, std::move( name ) );
            if( !built.has_value() )
            {
                return reject( scope, stepsPointer, built.error().message );
            }

            log::nominal(
                [&origin, &built, &json]( auto& event )
                {
                    event.tag( log::tags::sequence )
                        .value( "parsed",
                                origin.empty() ? std::string_view{ "<memory>" }
                                               : origin )
                        .value( "bytes", json.size() )
                        .value( "steps", built->steps().size() );
                }
            );

            return std::move( *built );
        }

    }    // namespace

    grab::Result<Sequence>
    parse( std::string_view json )
    {
        return parse_document( json, "" );
    }

    grab::Result<Sequence>
    load( const std::filesystem::path& path )
    {
        std::ifstream input{ path, std::ios::binary };
        if( !input.is_open() )
        {
            std::error_code error;
            const bool      missing = !std::filesystem::exists( path, error ) && !error;
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               path.string() +
                                   "/: " +
                                   ( missing ? "file not found" : "cannot open file" ) );
        }

        const std::string text{
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{}
        };
        return parse_document( text, path.string() + ": " );
    }

    grab::Result<std::string>
    to_json( const Sequence& sequence )
    {
        Json document                            = Json::object();
        document[json_key( fieldSchemaVersion )] = supportedSchemaVersion;
        if( !sequence.name().empty() )
        {
            document[json_key( fieldSequence )] = std::string{ sequence.name() };
        }

        const auto pacing = sequence.pacing();
        if( pacing.mode !=
            grab::sequence::PacingMode::Strict ||
            pacing.grace != std::chrono::milliseconds::zero() )
        {
            Json written = Json::object();
            written[json_key( fieldMode )] =
                std::string{ grab::sequence::pacing_mode_name( pacing.mode ) };
            written[json_key( fieldGraceMs )] = pacing.grace.count();
            document[json_key( fieldPacing )] = std::move( written );
        }

        const auto view  = sequence.steps();
        Json       array = Json::array();
        for( std::size_t index = 0U; index < view.size(); ++index )
        {
            const auto& step    = view[index];
            Json        written = Json::object();
            if( !step.label.empty() )
            {
                written[json_key( fieldId )] = step.label;
            }
            written[json_key( fieldOp )] = std::string{
                grab::command_name( grab::sequence::kind_of( step.command ) )
            };

            const auto payload = std::visit(
                [&written]( const auto& command )
                {
                    return write_payload( written, command );
                },
                step.command
            );
            if( !payload.has_value() )
            {
                return std::unexpected( payload.error() );
            }

            const auto edges = write_after( written, view, index, step );
            if( !edges.has_value() )
            {
                return std::unexpected( edges.error() );
            }

            if( step.on_error == grab::sequence::ErrorPolicy::Goto )
            {
                written[json_key( fieldOnError )] =
                    std::string{ gotoPrefix } + step.on_error_target;
            }
            else if( step.on_error != grab::sequence::ErrorPolicy::Abort )
            {
                written[json_key( fieldOnError )] =
                    std::string{ grab::sequence::error_policy_name( step.on_error ) };
            }

            if( step.extra_grace != std::chrono::milliseconds::zero() )
            {
                written[json_key( fieldExtraGraceMs )] = step.extra_grace.count();
            }

            array.push_back( std::move( written ) );
        }
        document[json_key( fieldSteps )] = std::move( array );

        return document.dump( jsonIndentWidth );
    }

}    // namespace grab::kernel::sequence
