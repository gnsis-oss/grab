#pragma once

#include "event/source.hpp"
#include "grab/event.hpp"
#include "grab/result.hpp"

#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace grab::event
{

    template<typename Monitor>
    class MonitorSource final : public EventSource
    {
        public:

            static_assert( std::is_move_constructible_v<Monitor> );
            static_assert( noexcept( std::declval<Monitor&>().stop() ) );

            using Factory = std::function<grab::Result<Monitor>( grab::core::Reactor&,
                                                                 grab::EventBus& )>;

            MonitorSource( std::string_view                 name,
                           std::span<const grab::EventKind> kinds,
                           Factory                          factory ) :
                name_( name ),
                kinds_( kinds ),
                factory_( std::move( factory ) )
            {
            }

            ~MonitorSource() override             = default;

            MonitorSource( const MonitorSource& ) = delete;
            MonitorSource&
            operator=( const MonitorSource& ) = delete;
            MonitorSource( MonitorSource&& )  = delete;
            MonitorSource&
            operator=( MonitorSource&& ) = delete;

            [[nodiscard]]
            grab::Result<void>
            start( grab::core::Reactor& reactor,
                   grab::EventBus&      bus ) override
            {
                auto opened = factory_( reactor, bus );
                if( !opened.has_value() )
                {
                    state_ = SourceState::Failed;
                    return std::unexpected( std::move( opened.error() ) );
                }

                monitor_.emplace( std::move( *opened ) );
                state_ = SourceState::Running;
                return {};
            }

            void
            stop() noexcept override
            {
                if( monitor_.has_value() )
                {
                    monitor_->stop();
                    monitor_.reset();
                }

                if( state_ == SourceState::Running )
                {
                    state_ = SourceState::Stopped;
                }
            }

            [[nodiscard]]
            SourceState
            state() const noexcept override
            {
                return state_;
            }

            [[nodiscard]]
            std::string_view
            name() const noexcept override
            {
                return name_;
            }

            [[nodiscard]]
            std::span<const grab::EventKind>
            kinds() const noexcept override
            {
                return kinds_;
            }

        private:

            std::string                      name_;
            std::span<const grab::EventKind> kinds_;
            Factory                          factory_;
            std::optional<Monitor>           monitor_;
            SourceState                      state_ = SourceState::Idle;
    };

}    // namespace grab::event
