#pragma once

#include "event/source.hpp"
#include "grab/event.hpp"
#include "grab/result.hpp"

#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace grab::event
{

    class SourceRegistry
    {
        public:

            struct Status
            {
                    std::string_view name;
                    SourceState      state = SourceState::Idle;
            };

            SourceRegistry()                        = default;
            ~SourceRegistry()                       = default;

            SourceRegistry( const SourceRegistry& ) = delete;
            SourceRegistry&
            operator=( const SourceRegistry& ) = delete;
            SourceRegistry( SourceRegistry&& ) = delete;
            SourceRegistry&
            operator=( SourceRegistry&& ) = delete;

            void
            add( std::unique_ptr<EventSource> source );

            [[nodiscard]]
            grab::Result<void>
            start_all( grab::core::Reactor& reactor,
                       grab::EventBus&      bus );

            void
            stop_all() noexcept;

            [[nodiscard]]
            bool
            is_kind_active( grab::EventKind kind ) const noexcept;

            [[nodiscard]]
            std::vector<Status>
            statuses() const;

        private:

            mutable std::mutex                        mutex_;
            std::vector<std::unique_ptr<EventSource>> sources_;
    };

}    // namespace grab::event
