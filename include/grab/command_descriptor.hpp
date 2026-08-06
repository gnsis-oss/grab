#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/enum_table.hpp"
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
            std::string_view name;
            CommandKind      kind;
            RetryClass       retry;
            Mutability       mutability;
            bool             idempotent;
            bool             consent_gated;
    };

    namespace detail
    {

        [[nodiscard]]
        constexpr CommandDescriptor
        command_descriptor( std::string_view name,
                            CommandKind      kind,
                            RetryClass       retry,
                            Mutability       mutability,
                            bool             idempotent,
                            bool             consent_gated ) noexcept
        {
            return CommandDescriptor{
                .name          = name,
                .kind          = kind,
                .retry         = retry,
                .mutability    = mutability,
                .idempotent    = idempotent,
                .consent_gated = consent_gated,
            };
        }

        inline constexpr auto commandDescriptors = std::to_array<CommandDescriptor>( {
            command_descriptor( "system.doctor",
                                CommandKind::Doctor,
                                RetryClass::Idempotent,
                                Mutability::ReadOnly,
                                true,
                                false ),
            command_descriptor( "service.daemon",
                                CommandKind::Daemon,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                false ),
            command_descriptor( "input.type",
                                CommandKind::Type,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                true ),
            command_descriptor( "input.click",
                                CommandKind::Click,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                true ),
            command_descriptor( "input.drag",
                                CommandKind::Drag,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                true ),
            command_descriptor( "input.drag_curve",
                                CommandKind::DragCurve,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                true ),
            command_descriptor( "screen.capture",
                                CommandKind::Capture,
                                RetryClass::Idempotent,
                                Mutability::ReadOnly,
                                true,
                                true ),
            command_descriptor( "screen.windows",
                                CommandKind::Windows,
                                RetryClass::Idempotent,
                                Mutability::ReadOnly,
                                true,
                                true ),
            // Activation converges on the same end state however often it runs,
            // so it is idempotent despite mutating the desktop's focus.
            command_descriptor( "window.focus",
                                CommandKind::Focus,
                                RetryClass::Idempotent,
                                Mutability::Mutating,
                                true,
                                true ),
            // Placement verifies that the geometry it asked for was reached, so a
            // repeat run is a no-op that re-confirms the same end state.
            command_descriptor( "window.place",
                                CommandKind::Place,
                                RetryClass::Idempotent,
                                Mutability::Mutating,
                                true,
                                true ),
            command_descriptor( "screen.batch",
                                CommandKind::Batch,
                                RetryClass::Idempotent,
                                Mutability::ReadOnly,
                                true,
                                true ),
            command_descriptor( "image.compare",
                                CommandKind::Compare,
                                RetryClass::Idempotent,
                                Mutability::ReadOnly,
                                true,
                                false ),
            command_descriptor( "screen.watch",
                                CommandKind::Watch,
                                RetryClass::Idempotent,
                                Mutability::ReadOnly,
                                true,
                                true ),
            command_descriptor( "input.key",
                                CommandKind::Key,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                true ),
            command_descriptor( "session.open",
                                CommandKind::Session,
                                RetryClass::Never,
                                Mutability::Mutating,
                                false,
                                true ),
            command_descriptor( "overlay.trail",
                                CommandKind::OverlayTrail,
                                RetryClass::ResolveOnly,
                                Mutability::Mutating,
                                false,
                                false ),
            command_descriptor( "overlay.shape",
                                CommandKind::OverlayShape,
                                RetryClass::ResolveOnly,
                                Mutability::Mutating,
                                false,
                                false ),
            command_descriptor( "overlay.feedback",
                                CommandKind::OverlayFeedback,
                                RetryClass::ResolveOnly,
                                Mutability::Mutating,
                                false,
                                false ),
            command_descriptor( "overlay.sketch",
                                CommandKind::OverlaySketch,
                                RetryClass::ResolveOnly,
                                Mutability::Mutating,
                                false,
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
    constexpr const auto&
    list_commands() noexcept
    {
        return detail::commandDescriptors;
    }

}    // namespace grab
