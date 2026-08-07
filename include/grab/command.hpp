#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// The closed command variant a sequence step carries, and the Step that holds
// it.
//
// Commands are a closed std::variant, matching grab::Action and the flat-memory
// rule: a third party cannot add a command type without touching this variant,
// which is consistent with CommandKind already being a closed enum.
//
// THE 23-OF-38 GAP, which the interpreter depends on:
//
// The CommandDescriptor table names 38 commands. Only the 23 alternatives
// below are meaningful as sequence steps. `system.doctor`, `service.daemon`,
// `screen.watch`, `session.open`, `screen.batch`, `image.compare`,
// `input.drag_curve`, `screen.windows`, `window.focus`, `window.place`,
// `system.play` and the four `overlay.*` CLI verbs (trail, shape, feedback,
// sketch) resolve through command_kind() but have NO payload struct here. The
// interpreter must reject them as "op X is not available as a sequence step" —
// a DIFFERENT message from "unknown op X", because they are different author
// mistakes.
//
// The eight overlay STEPS below are a different set from those four verbs and
// share none of their names: overlay.add/update/remove/clear/grab/release/
// attach/detach are payload-carrying steps, overlay.trail/shape/feedback/sketch
// are interactive tools with no payload at all.

#include "grab/command_descriptor.hpp"
#include "grab/drag.hpp"
#include "grab/enum_table.hpp"
#include "grab/geometry/curve.hpp"
#include "grab/geometry/point.hpp"
#include "grab/overlay.hpp"
#include "grab/pointer_button.hpp"
#include "grab/sequence_types.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace grab::sequence
{

    struct TypeCommand
    {
            static constexpr CommandKind commandKind = CommandKind::Type;

            // Every member carries a default initializer so that designated
            // initialization stays usable: -Wextra turns
            // -Wmissing-designated-field-initializers on, and -Werror makes a
            // skipped field without one a build failure at every call site.
            std::string                  text{};
    };

    struct KeyCommand
    {
            static constexpr CommandKind commandKind = CommandKind::Key;

            std::string                  key{};
    };

    struct KeyDownCommand
    {
            static constexpr CommandKind commandKind = CommandKind::KeyDown;

            std::string                  key{};
    };

    struct KeyUpCommand
    {
            static constexpr CommandKind commandKind = CommandKind::KeyUp;

            std::string                  key{};
    };

    struct ClickCommand
    {
            static constexpr CommandKind commandKind = CommandKind::Click;

            std::uint8_t                 button{ grab::input::primaryButton };
    };

    struct ClickAtCommand
    {
            static constexpr CommandKind commandKind = CommandKind::ClickAt;

            grab::geometry::Point        at{};
            std::uint8_t                 button{ grab::input::primaryButton };
    };

    struct PressCommand
    {
            static constexpr CommandKind commandKind = CommandKind::Press;

            std::uint8_t                 button{ grab::input::primaryButton };
    };

    struct ReleaseCommand
    {
            static constexpr CommandKind commandKind = CommandKind::Release;

            std::uint8_t                 button{ grab::input::primaryButton };
    };

    // Notches, not pixels: positive dy scrolls DOWN and positive dx RIGHT.
    struct ScrollCommand
    {
            static constexpr CommandKind commandKind = CommandKind::Scroll;

            std::int32_t                 dx{ 0 };
            std::int32_t                 dy{ 0 };
    };

    // A single warp — today's Input::move(). No interpolation, no button.
    struct WarpCommand
    {
            static constexpr CommandKind commandKind = CommandKind::Warp;

            grab::geometry::Point        to{};
    };

    // Interpolated motion with no button held. `from` is nullopt when the
    // author did not say, meaning "wherever the pointer already is" — which is
    // only knowable at run time.
    struct MoveCommand
    {
            static constexpr CommandKind         commandKind = CommandKind::Move;

            std::optional<grab::geometry::Point> from{};
            grab::geometry::Point                to{};
            grab::input::DragOptions             options{};
    };

    // The same walk along a curve, no button held.
    struct FollowCommand
    {
            static constexpr CommandKind commandKind = CommandKind::Follow;

            grab::geometry::Curve        path{};
            grab::input::DragOptions     options{};
    };

    struct DragCommand
    {
            static constexpr CommandKind commandKind = CommandKind::Drag;

            grab::geometry::Point        from{};
            grab::geometry::Point        to{};
            std::uint8_t                 button{ grab::input::primaryButton };
            grab::input::DragOptions     options{};
    };

    // Exactly one of `output` and `locator` carries a value, matching the
    // capture verb's own precondition.
    struct CaptureCommand
    {
            static constexpr CommandKind commandKind = CommandKind::Capture;

            std::string                  output{};
            std::string                  locator{};
    };

    // The one op whose declared duration is mandatory in JSON.
    struct WaitCommand
    {
            static constexpr CommandKind commandKind = CommandKind::Wait;

            std::chrono::nanoseconds     duration{};
    };

    // ── Overlay steps ────────────────────────────────────────
    //
    // `handle` is a DOCUMENT-LEVEL NAME, exactly like a step label: a readable
    // string the run state maps to an overlay::ShapeId. It is not the identity
    // and it is never serialized as one. An overlay.add with an empty handle is
    // fire-and-forget — drawable, never referenced again.
    //
    // Nothing here carries a ShapeId, because a document is written before any
    // scene exists. Resolving handle to ShapeId is run state, and belongs to
    // whatever runs the sequence.

    struct OverlayAddCommand
    {
            static constexpr CommandKind commandKind = CommandKind::OverlayAdd;

            std::string                  handle{};
            grab::overlay::Shape         shape{};
    };

    struct OverlayUpdateCommand
    {
            static constexpr CommandKind commandKind = CommandKind::OverlayUpdate;

            std::string                  handle{};
            grab::overlay::Shape         shape{};
    };

    // A ttl or fade lifetime expires a shape from the scene itself, so a
    // remove may find nothing. That is a no-op, not an error: a document that
    // wants exact add/remove accounting uses a persistent lifetime.
    struct OverlayRemoveCommand
    {
            static constexpr CommandKind commandKind = CommandKind::OverlayRemove;

            std::string                  handle{};
    };

    struct OverlayClearCommand
    {
            static constexpr CommandKind commandKind = CommandKind::OverlayClear;
    };

    // Overlay::capture_pointer, with every rule it carries: arm it when the
    // tool becomes armed rather than at button-press, and THE CALLER OWNS THE
    // CAPTURE — a pointer grab that outlives its owner freezes the desktop, so
    // the unwind path must release it.
    struct OverlayGrabCommand
    {
            static constexpr CommandKind commandKind = CommandKind::OverlayGrab;
    };

    struct OverlayReleaseCommand
    {
            static constexpr CommandKind commandKind = CommandKind::OverlayRelease;
    };

    // The shape rides the pointer until it is detached. `offset` absent means
    // "keep the gap the shape already has" — the shape's position minus the
    // pointer's at attach time, which is only knowable at run time — so a
    // square picked up by its corner stays held by that corner.
    struct OverlayAttachCommand
    {
            static constexpr CommandKind commandKind = CommandKind::OverlayAttach;

            std::string                  handle{};
            std::optional<grab::geometry::Point> offset{};
    };

    struct OverlayDetachCommand
    {
            static constexpr CommandKind commandKind = CommandKind::OverlayDetach;

            std::string                  handle{};
    };

    using Command = std::variant<TypeCommand,
                                 KeyCommand,
                                 KeyDownCommand,
                                 KeyUpCommand,
                                 ClickCommand,
                                 ClickAtCommand,
                                 PressCommand,
                                 ReleaseCommand,
                                 ScrollCommand,
                                 WarpCommand,
                                 MoveCommand,
                                 FollowCommand,
                                 DragCommand,
                                 CaptureCommand,
                                 WaitCommand,
                                 OverlayAddCommand,
                                 OverlayUpdateCommand,
                                 OverlayRemoveCommand,
                                 OverlayClearCommand,
                                 OverlayGrabCommand,
                                 OverlayReleaseCommand,
                                 OverlayAttachCommand,
                                 OverlayDetachCommand>;

    inline constexpr std::size_t sequenceCommandCount = 23U;
    static_assert( std::variant_size_v<Command> == sequenceCommandCount );

    [[nodiscard]]
    inline CommandKind
    kind_of( const Command& command ) noexcept
    {
        return std::visit(
            []( const auto& payload ) noexcept
            {
                return std::remove_cvref_t<decltype( payload )>::commandKind;
            },
            command
        );
    }

    // True for exactly the kinds that have a payload struct above. The
    // interpreter uses this to tell "not available as a sequence step" from
    // "unknown op".
    [[nodiscard]]
    constexpr bool
    is_sequence_command( CommandKind kind ) noexcept
    {
        switch( kind )
        {
            case CommandKind::Type :
            case CommandKind::Key :
            case CommandKind::KeyDown :
            case CommandKind::KeyUp :
            case CommandKind::Click :
            case CommandKind::ClickAt :
            case CommandKind::Press :
            case CommandKind::Release :
            case CommandKind::Scroll :
            case CommandKind::Warp :
            case CommandKind::Move :
            case CommandKind::Follow :
            case CommandKind::Drag :
            case CommandKind::Capture :
            case CommandKind::Wait :
            case CommandKind::OverlayAdd :
            case CommandKind::OverlayUpdate :
            case CommandKind::OverlayRemove :
            case CommandKind::OverlayClear :
            case CommandKind::OverlayGrab :
            case CommandKind::OverlayRelease :
            case CommandKind::OverlayAttach :
            case CommandKind::OverlayDetach :
                return true;
            case CommandKind::Doctor :
            case CommandKind::Daemon :
            case CommandKind::DragCurve :
            case CommandKind::Windows :
            case CommandKind::Focus :
            case CommandKind::Place :
            case CommandKind::Batch :
            case CommandKind::Compare :
            case CommandKind::Watch :
            case CommandKind::Session :
            case CommandKind::OverlayTrail :
            case CommandKind::OverlayShape :
            case CommandKind::OverlayFeedback :
            case CommandKind::OverlaySketch :
            case CommandKind::Play :
            case CommandKind::Count :
                return false;
        }
        return false;
    }

    // What a step does when its command fails. Abort is the default: a
    // sequence that keeps going after an unexplained failure produces garbage
    // input into an application in an unknown state.
    enum class ErrorPolicy : std::uint8_t
    {
        Abort,
        Continue,
        Goto,
        Count,
    };

    namespace detail
    {

        inline constexpr auto errorPolicyNames = EnumTable{
            std::to_array( {
                enum_entry( ErrorPolicy::Abort, "abort" ),
                enum_entry( ErrorPolicy::Continue, "continue" ),
                enum_entry( ErrorPolicy::Goto, "goto" ),
            } ),
        };
        static_assert( enum_table_has_count( errorPolicyNames,
                                             ErrorPolicy::Count ) );

    }    // namespace detail

    [[nodiscard]]
    constexpr std::string_view
    error_policy_name( ErrorPolicy value ) noexcept
    {
        return detail::errorPolicyNames.text_of( value, "" );
    }

    [[nodiscard]]
    constexpr std::optional<ErrorPolicy>
    error_policy_from_name( std::string_view text ) noexcept
    {
        return detail::errorPolicyNames.value_of( text );
    }

    // One node of the document.
    //
    // `label` is an OPTIONAL AUTHOR HANDLE, not the identity — `id` is. Two
    // unlabelled, byte-identical clicks are distinct steps by construction,
    // with no uniqueness check to forget.
    //
    // `after` holds resolved ids rather than labels, because a step with no
    // label must still be nameable as a predecessor: the implicit
    // depends-on-the-previous-step edge points at whatever came before,
    // labelled or not.
    //
    // `extra_grace` is read ONLY under PacingMode::Precise. Under Strict and
    // Grace it loads and is ignored, so one document runs under all three
    // modes without being edited.
    struct Step
    {
            StepId                    id{};
            std::string               label{};
            Command                   command{};
            std::vector<StepId>       after{};
            ErrorPolicy               on_error{ ErrorPolicy::Abort };
            std::string               on_error_target{};
            std::chrono::milliseconds extra_grace{ std::chrono::milliseconds::zero() };
    };

}    // namespace grab::sequence
