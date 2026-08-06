#include "grab/command.hpp"
#include "grab/command_descriptor.hpp"
#include "grab/drag.hpp"
#include "grab/geometry/curve.hpp"
#include "grab/geometry/point.hpp"
#include "grab/pointer_button.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "kernel/sequence/interpreter.hpp"
#include "kernel/sequence/sequence.hpp"
#include "kernel/support/log.hpp"
#include "kernel/support/log_tags.hpp"

#include <algorithm>
#include <array>
#include <chrono>
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
