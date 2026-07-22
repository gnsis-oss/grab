#pragma once

#include "grab/enum_table.hpp"
#include "grab/geometry/size.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace grab
{

    class Workspace
    {
    };

    enum class WorkspaceMode : std::uint8_t
    {
        Shared,
        Offscreen,
        Count,
    };

    enum class WorkspaceState : std::uint8_t
    {
        Starting,
        Ready,
        Draining,
        Stopped,
        Failed,
        Count,
    };

    using WorkspaceGeometry = geometry::Size;

    struct WorkspaceDesc
    {
            std::string       name;
            WorkspaceMode     mode = WorkspaceMode::Offscreen;
            WorkspaceGeometry geometry;
            std::string       app_command;    // empty = launch nothing
    };

    namespace detail
    {

        inline constexpr auto workspace_mode_names = EnumTable{
            std::to_array( {
                enum_entry( WorkspaceMode::Shared, "shared" ),
                enum_entry( WorkspaceMode::Offscreen, "offscreen" ),
            } ),
        };
        static_assert( enum_table_has_count( workspace_mode_names,
                                             WorkspaceMode::Count ) );

        inline constexpr auto workspace_state_names = EnumTable{
            std::to_array( {
                enum_entry( WorkspaceState::Starting, "starting" ),
                enum_entry( WorkspaceState::Ready, "ready" ),
                enum_entry( WorkspaceState::Draining, "draining" ),
                enum_entry( WorkspaceState::Stopped, "stopped" ),
                enum_entry( WorkspaceState::Failed, "failed" ),
            } ),
        };
        static_assert( enum_table_has_count( workspace_state_names,
                                             WorkspaceState::Count ) );

    }    // namespace detail

    [[nodiscard]]
    constexpr std::string_view
    mode_name( WorkspaceMode mode ) noexcept
    {
        return detail::workspace_mode_names.text_of( mode, "offscreen" );
    }

    [[nodiscard]]
    constexpr std::optional<WorkspaceMode>
    mode_from_string( std::string_view text ) noexcept
    {
        return detail::workspace_mode_names.value_of( text );
    }

    [[nodiscard]]
    constexpr std::string_view
    state_name( WorkspaceState state ) noexcept
    {
        return detail::workspace_state_names.text_of( state, "failed" );
    }

    [[nodiscard]]
    constexpr std::optional<WorkspaceState>
    session_state_from_string( std::string_view text ) noexcept
    {
        return detail::workspace_state_names.value_of( text );
    }

}    // namespace grab
