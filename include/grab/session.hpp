#pragma once

#include "grab/enum_table.hpp"
#include "grab/geometry/size.hpp"
#include "grab/result.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace grab::core
{

    class Reactor;

}    // namespace grab::core

namespace grab
{

    struct SessionOptions
    {
            std::optional<std::string> display;
            std::optional<std::string> seat;
    };

    class Session
    {
        public:

            [[nodiscard]]
            static grab::Result<std::unique_ptr<Session>>
            open( SessionOptions options = {} );

            ~Session();

            Session( const Session& ) = delete;
            Session&
            operator=( const Session& ) = delete;
            Session( Session&& )        = delete;
            Session&
            operator=( Session&& ) = delete;

            void
            close() noexcept;

            [[nodiscard]]
            bool
            is_open() const noexcept;

            [[nodiscard]]
            grab::core::Reactor&
            reactor() noexcept;

            [[nodiscard]]
            grab::Result<void>
            post( std::function<void()> fn );

        private:

            explicit Session( SessionOptions options );

            class Impl;

            std::unique_ptr<Impl> impl_;
    };

    // ── Session descriptors (integrated session/inventory subsystem) ─────────
    // Distinct from the live `Session` object above: these describe a session
    // to the SessionProvider/SessionManager lifecycle.

    enum class SessionMode : std::uint8_t
    {
        shared,
        offscreen,
        count,
    };

    enum class SessionState : std::uint8_t
    {
        starting,
        ready,
        draining,
        stopped,
        failed,
        count,
    };

    using SessionGeometry = geometry::Size;

    struct SessionDesc
    {
            std::string     name;
            SessionMode     mode = SessionMode::offscreen;
            SessionGeometry geometry;
            std::string     app_command;    // empty = launch nothing
    };

    namespace detail
    {

        inline constexpr auto session_mode_names = EnumTable{
            std::to_array( {
                enum_entry( SessionMode::shared, "shared" ),
                enum_entry( SessionMode::offscreen, "offscreen" ),
            } ),
        };
        static_assert( enum_table_has_count( session_mode_names,
                                             SessionMode::count ) );

        inline constexpr auto session_state_names = EnumTable{
            std::to_array( {
                enum_entry( SessionState::starting, "starting" ),
                enum_entry( SessionState::ready, "ready" ),
                enum_entry( SessionState::draining, "draining" ),
                enum_entry( SessionState::stopped, "stopped" ),
                enum_entry( SessionState::failed, "failed" ),
            } ),
        };
        static_assert( enum_table_has_count( session_state_names,
                                             SessionState::count ) );

    }    // namespace detail

    [[nodiscard]]
    constexpr std::string_view
    mode_name( SessionMode mode ) noexcept
    {
        return detail::session_mode_names.text_of( mode, "offscreen" );
    }

    [[nodiscard]]
    constexpr std::optional<SessionMode>
    mode_from_string( std::string_view text ) noexcept
    {
        return detail::session_mode_names.value_of( text );
    }

    [[nodiscard]]
    constexpr std::string_view
    state_name( SessionState state ) noexcept
    {
        return detail::session_state_names.text_of( state, "failed" );
    }

    [[nodiscard]]
    constexpr std::optional<SessionState>
    session_state_from_string( std::string_view text ) noexcept
    {
        return detail::session_state_names.value_of( text );
    }

}    // namespace grab
