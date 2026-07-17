#pragma once

#include "grab/capture.hpp"
#include "grab/interaction.hpp"
#include "grab/locator.hpp"
#include "grab/query.hpp"
#include "grab/result.hpp"
#include "grab/trace.hpp"
#include "grab/watch.hpp"
#include "grab/workspace.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace grab::core
{

    class Reactor;

}    // namespace grab::core

namespace grab::spi
{

    class Runtime;

}    // namespace grab::spi

namespace grab
{

    class EventBus;

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

            // Compose a session driven by an externally-provided runtime instead
            // of the default display stack (observation/daemon seam).
            [[nodiscard]]
            static grab::Result<std::unique_ptr<Session>>
            open_owning_runtime( std::unique_ptr<grab::spi::Runtime> runtime );

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

            // The session's live event bus (the composed core's bus, or a
            // reactor-only fallback bus when no runtime is composed).
            [[nodiscard]]
            EventBus&
            bus() noexcept;

            // Start/stop continuous observation over the composed runtime's
            // event source and tree deltas. No-op when no runtime is composed.
            [[nodiscard]]
            grab::Result<void>
            start_observation();

            void
            stop_observation() noexcept;

            [[nodiscard]]
            Result<Match>
            resolve( const Locator& locator,
                     Cardinality    cardinality = Cardinality::ExactlyOne );

            [[nodiscard]]
            Result<NodeInfo>
            describe( const Match& match );

            [[nodiscard]]
            Result<Subscription>
            watch( SubscriptionScope scope,
                   QueueOptions      options = {} );

            [[nodiscard]]
            Result<Receipt>
            perform( const Action&        action,
                     const ActionOptions& options = {} );

            [[nodiscard]]
            Result<Frame>
            capture( const CaptureTarget& target,
                     CaptureOptions       options = {} );

        private:

            explicit Session( SessionOptions options );

            class Impl;

            std::unique_ptr<Impl> impl_;
    };

    using SessionDesc [[deprecated( "use WorkspaceDesc" )]]         = WorkspaceDesc;
    using SessionMode [[deprecated( "use WorkspaceMode" )]]         = WorkspaceMode;
    using SessionState [[deprecated( "use WorkspaceState" )]]       = WorkspaceState;
    using SessionGeometry [[deprecated( "use WorkspaceGeometry" )]] = WorkspaceGeometry;

}    // namespace grab
