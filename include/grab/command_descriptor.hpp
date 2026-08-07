#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/enum_table.hpp"
#include "grab/sequence_types.hpp"
#include "grab/trace.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace grab
{

    enum class CommandKind : std::uint8_t
    {
        Doctor,
        Daemon,
        Type,
        Click,
        Drag,
        DragCurve,
        Capture,
        Windows,
        Focus,
        Place,
        Batch,
        Compare,
        Watch,
        Key,
        Session,
        OverlayTrail,
        OverlayShape,
        OverlayFeedback,
        OverlaySketch,
        // Sequence-era additions. Move/Warp is a real distinction the library
        // does not draw: Input::move() is a teleport, and interpolation only
        // ever existed welded to a button press inside execute_drag.
        Move,
        Warp,
        Follow,
        Press,
        Release,
        Scroll,
        ClickAt,
        KeyDown,
        KeyUp,
        Wait,
        Play,
        // Overlay steps. The variant covered input, capture and wait only, so a
        // sequence could move the pointer onto a target and click it but could
        // not place, move or remove the target. These eight close that, and
        // they are NOT the four overlay.* CLI verbs above — trail, shape,
        // feedback and sketch are whole interactive tools with no payload.
        OverlayAdd,
        OverlayUpdate,
        OverlayRemove,
        OverlayClear,
        OverlayGrab,
        OverlayRelease,
        OverlayAttach,
        OverlayDetach,
        Count,
    };

    enum class Mutability : std::uint8_t
    {
        ReadOnly,
        Mutating,
        Count,
    };

    struct CommandDescriptor
    {
            std::string_view      name;
            CommandKind           kind;
            RetryClass            retry;
            Mutability            mutability;
            bool                  idempotent;
            bool                  consent_gated;
            // Where the step's duration comes from, and whether its body must
            // run on a worker rather than on the thread that owns deadlines.
            // Blocking work sharing the timing thread is the dominant
            // precision ceiling: a synchronous capture slips every deadline in
            // the frontier by its full duration.
            sequence::TimingClass timing;
            bool                  blocking;
    };

    namespace detail
    {

        [[nodiscard]]
        constexpr CommandDescriptor
        command_descriptor( std::string_view      name,
                            CommandKind           kind,
                            RetryClass            retry,
                            Mutability            mutability,
                            bool                  idempotent,
                            bool                  consent_gated,
                            sequence::TimingClass timing,
                            bool                  blocking ) noexcept
        {
            return CommandDescriptor{
                .name          = name,
                .kind          = kind,
                .retry         = retry,
                .mutability    = mutability,
                .idempotent    = idempotent,
                .consent_gated = consent_gated,
                .timing        = timing,
                .blocking      = blocking,
            };
        }

        inline constexpr auto commandDescriptors = std::to_array<CommandDescriptor>( {
            command_descriptor( "system.doctor",
                                CommandKind::Doctor,
                                RetryClass::Idempotent,
                                Mutability::ReadOnly,
                                true,
                                false,
                                sequence::TimingClass::Opaque,
                                true ),
            command_descriptor( "service.daemon",
                                CommandKind::Daemon,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                false,
                                sequence::TimingClass::Opaque,
                                false ),
            command_descriptor( "input.type",
                                CommandKind::Type,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                true,
                                sequence::TimingClass::Instant,
                                false ),
            command_descriptor( "input.click",
                                CommandKind::Click,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                true,
                                sequence::TimingClass::Instant,
                                false ),
            command_descriptor( "input.drag",
                                CommandKind::Drag,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                true,
                                sequence::TimingClass::Timed,
                                false ),
            command_descriptor( "input.drag_curve",
                                CommandKind::DragCurve,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                true,
                                sequence::TimingClass::Timed,
                                false ),
            command_descriptor( "screen.capture",
                                CommandKind::Capture,
                                RetryClass::Idempotent,
                                Mutability::ReadOnly,
                                true,
                                true,
                                sequence::TimingClass::Opaque,
                                true ),
            command_descriptor( "screen.windows",
                                CommandKind::Windows,
                                RetryClass::Idempotent,
                                Mutability::ReadOnly,
                                true,
                                true,
                                sequence::TimingClass::Opaque,
                                true ),
            // Activation converges on the same end state however often it runs,
            // so it is idempotent despite mutating the desktop's focus.
            command_descriptor( "window.focus",
                                CommandKind::Focus,
                                RetryClass::Idempotent,
                                Mutability::Mutating,
                                true,
                                true,
                                sequence::TimingClass::Opaque,
                                false ),
            // Placement verifies that the geometry it asked for was reached, so a
            // repeat run is a no-op that re-confirms the same end state.
            command_descriptor( "window.place",
                                CommandKind::Place,
                                RetryClass::Idempotent,
                                Mutability::Mutating,
                                true,
                                true,
                                sequence::TimingClass::Opaque,
                                false ),
            command_descriptor( "screen.batch",
                                CommandKind::Batch,
                                RetryClass::Idempotent,
                                Mutability::ReadOnly,
                                true,
                                true,
                                sequence::TimingClass::Opaque,
                                true ),
            command_descriptor( "image.compare",
                                CommandKind::Compare,
                                RetryClass::Idempotent,
                                Mutability::ReadOnly,
                                true,
                                false,
                                sequence::TimingClass::Opaque,
                                true ),
            command_descriptor( "screen.watch",
                                CommandKind::Watch,
                                RetryClass::Idempotent,
                                Mutability::ReadOnly,
                                true,
                                true,
                                sequence::TimingClass::Opaque,
                                false ),
            command_descriptor( "input.key",
                                CommandKind::Key,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                true,
                                sequence::TimingClass::Instant,
                                false ),
            command_descriptor( "session.open",
                                CommandKind::Session,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                true,
                                sequence::TimingClass::Opaque,
                                false ),
            command_descriptor( "overlay.trail",
                                CommandKind::OverlayTrail,
                                RetryClass::ResolveOnly,
                                Mutability::Mutating,
                                false,
                                false,
                                sequence::TimingClass::Instant,
                                false ),
            command_descriptor( "overlay.shape",
                                CommandKind::OverlayShape,
                                RetryClass::ResolveOnly,
                                Mutability::Mutating,
                                false,
                                false,
                                sequence::TimingClass::Instant,
                                false ),
            command_descriptor( "overlay.feedback",
                                CommandKind::OverlayFeedback,
                                RetryClass::ResolveOnly,
                                Mutability::Mutating,
                                false,
                                false,
                                sequence::TimingClass::Instant,
                                false ),
            command_descriptor( "overlay.sketch",
                                CommandKind::OverlaySketch,
                                RetryClass::ResolveOnly,
                                Mutability::Mutating,
                                false,
                                false,
                                sequence::TimingClass::Instant,
                                false ),
            // Interpolated pointer motion with no button held. Distinct from
            // input.warp, which is today's Input::move(): a single teleport.
            command_descriptor( "input.move",
                                CommandKind::Move,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                true,
                                sequence::TimingClass::Timed,
                                false ),
            command_descriptor( "input.warp",
                                CommandKind::Warp,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                true,
                                sequence::TimingClass::Instant,
                                false ),
            command_descriptor( "input.follow",
                                CommandKind::Follow,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                true,
                                sequence::TimingClass::Timed,
                                false ),
            // press/release exist so a caller can hold a button for a
            // realistic 50-150 ms; click() holds for however long two XTEST
            // requests take, which is far shorter than a human's.
            command_descriptor( "input.press",
                                CommandKind::Press,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                true,
                                sequence::TimingClass::Instant,
                                false ),
            command_descriptor( "input.release",
                                CommandKind::Release,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                true,
                                sequence::TimingClass::Instant,
                                false ),
            command_descriptor( "input.scroll",
                                CommandKind::Scroll,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                true,
                                sequence::TimingClass::Instant,
                                false ),
            command_descriptor( "input.click_at",
                                CommandKind::ClickAt,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                true,
                                sequence::TimingClass::Instant,
                                false ),
            // Without key_down/key_up no declarative surface can express a
            // chord: Keystroke carries only shift and altgr, so Ctrl+C is
            // unreachable through type/key.
            command_descriptor( "input.key_down",
                                CommandKind::KeyDown,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                true,
                                sequence::TimingClass::Instant,
                                false ),
            command_descriptor( "input.key_up",
                                CommandKind::KeyUp,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                true,
                                sequence::TimingClass::Instant,
                                false ),
            // The only op whose declared duration is mandatory in JSON; every
            // other Timed op takes its dwell from defaulted options.
            command_descriptor( "time.wait",
                                CommandKind::Wait,
                                RetryClass::Idempotent,
                                Mutability::ReadOnly,
                                true,
                                false,
                                sequence::TimingClass::Timed,
                                false ),
            command_descriptor( "system.play",
                                CommandKind::Play,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                true,
                                sequence::TimingClass::Opaque,
                                false ),
            // The overlay steps. All eight are Instant and none is blocking:
            // measured over 471 calls, a mutation issued from the reactor
            // thread averages 0.02 ms and add_many of 56 shapes costs 1.1 ms.
            // The frame is paid by flush(), which the player issues per tick
            // rather than per step, so no overlay step owns a frame's latency.
            //
            // remove/clear/release/detach are Idempotent because they converge
            // on the same end state however often they run — removing an
            // already-removed handle, releasing an ungrabbed pointer and
            // detaching an unattached shape are all no-ops that succeed. add,
            // update, attach and grab are Never: a second add draws a second
            // shape, and re-grabbing a pointer this process already owns is a
            // different question from grabbing it once.
            command_descriptor( "overlay.add",
                                CommandKind::OverlayAdd,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                false,
                                sequence::TimingClass::Instant,
                                false ),
            command_descriptor( "overlay.update",
                                CommandKind::OverlayUpdate,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                false,
                                sequence::TimingClass::Instant,
                                false ),
            command_descriptor( "overlay.remove",
                                CommandKind::OverlayRemove,
                                RetryClass::Idempotent,
                                Mutability::Mutating,
                                true,
                                false,
                                sequence::TimingClass::Instant,
                                false ),
            command_descriptor( "overlay.clear",
                                CommandKind::OverlayClear,
                                RetryClass::Idempotent,
                                Mutability::Mutating,
                                true,
                                false,
                                sequence::TimingClass::Instant,
                                false ),
            // THE CALLER OWNS THE CAPTURE: a pointer grab that outlives its
            // owner freezes the whole desktop, so the player's unwind path must
            // release it however the run ends.
            command_descriptor( "overlay.grab",
                                CommandKind::OverlayGrab,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                false,
                                sequence::TimingClass::Instant,
                                false ),
            command_descriptor( "overlay.release",
                                CommandKind::OverlayRelease,
                                RetryClass::Idempotent,
                                Mutability::Mutating,
                                true,
                                false,
                                sequence::TimingClass::Instant,
                                false ),
            command_descriptor( "overlay.attach",
                                CommandKind::OverlayAttach,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                false,
                                sequence::TimingClass::Instant,
                                false ),
            command_descriptor( "overlay.detach",
                                CommandKind::OverlayDetach,
                                RetryClass::Idempotent,
                                Mutability::Mutating,
                                true,
                                false,
                                sequence::TimingClass::Instant,
                                false ),
        } );

        template<std::size_t Size>
        struct CommandNameTable
        {
                [[nodiscard]]
                constexpr std::size_t
                size() const noexcept
                {
                    return Size;
                }

                [[nodiscard]]
                constexpr std::string_view
                text_of( CommandKind      kind,
                         std::string_view fallback ) const noexcept
                {
                    const auto* const descriptor =
                        std::ranges::find( commandDescriptors,
                                           kind,
                                           &CommandDescriptor::kind );
                    return descriptor == commandDescriptors.end() ? fallback
                                                                  : descriptor->name;
                }

                [[nodiscard]]
                constexpr std::optional<CommandKind>
                value_of( std::string_view name ) const noexcept
                {
                    const auto* const descriptor =
                        std::ranges::find( commandDescriptors,
                                           name,
                                           &CommandDescriptor::name );
                    if( descriptor == commandDescriptors.end() )
                    {
                        return std::nullopt;
                    }
                    return descriptor->kind;
                }
        };

        inline constexpr CommandNameTable<commandDescriptors.size()> commandNames{};
        static_assert( commandNames.size() ==
                       static_cast<std::size_t>( CommandKind::Count ) );

        inline constexpr auto mutabilityNames = EnumTable{
            std::to_array( {
                enum_entry( Mutability::ReadOnly, "read_only" ),
                enum_entry( Mutability::Mutating, "mutating" ),
            } ),
        };
        static_assert( enum_table_has_count( mutabilityNames,
                                             Mutability::Count ) );

        [[nodiscard]]
        constexpr bool
        command_names_are_unique() noexcept
        {
            return std::ranges::all_of(
                commandDescriptors,
                []( const CommandDescriptor& descriptor )
                {
                    return std::ranges::count( commandDescriptors,
                                               descriptor.name,
                                               &CommandDescriptor::name ) == 1;
                }
            );
        }

        [[nodiscard]]
        constexpr bool
        command_names_round_trip() noexcept
        {
            return std::ranges::all_of( commandDescriptors,
                                        []( const CommandDescriptor& descriptor )
                                        {
                                            const auto kind =
                                                commandNames.value_of( descriptor.name );
                                            return kind.has_value() &&
                                                 *kind ==
                                                   descriptor.kind &&
                                                   commandNames.text_of( descriptor.kind,
                                                                         "" ) ==
                                                   descriptor.name;
                                        } );
        }

        static_assert( command_names_are_unique() );
        static_assert( command_names_round_trip() );

    }    // namespace detail

    [[nodiscard]]
    constexpr std::string_view
    command_name( CommandKind kind ) noexcept
    {
        return detail::commandNames.text_of( kind, "" );
    }

    [[nodiscard]]
    constexpr std::optional<CommandKind>
    command_kind( std::string_view name ) noexcept
    {
        return detail::commandNames.value_of( name );
    }

    [[nodiscard]]
    constexpr std::string_view
    mutability_name( Mutability mutability ) noexcept
    {
        return detail::mutabilityNames.text_of( mutability, "" );
    }

    [[nodiscard]]
    constexpr sequence::TimingClass
    timing_class_of( CommandKind kind ) noexcept
    {
        const auto* const descriptor = std::ranges::find( detail::commandDescriptors,
                                                          kind,
                                                          &CommandDescriptor::kind );
        return descriptor == detail::commandDescriptors.end()
                 ? sequence::TimingClass::Opaque
                 : descriptor->timing;
    }

    [[nodiscard]]
    constexpr bool
    is_blocking_command( CommandKind kind ) noexcept
    {
        const auto* const descriptor = std::ranges::find( detail::commandDescriptors,
                                                          kind,
                                                          &CommandDescriptor::kind );
        return descriptor != detail::commandDescriptors.end() && descriptor->blocking;
    }

    [[nodiscard]]
    constexpr const auto&
    list_commands() noexcept
    {
        return detail::commandDescriptors;
    }

}    // namespace grab
