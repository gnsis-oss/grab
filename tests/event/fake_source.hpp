#pragma once

#include "event/source.hpp"
#include "grab/event.hpp"
#include "grab/result.hpp"

#include <cstddef>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab::test
{

    class FakeSource final : public grab::event::EventSource
    {
        public:

            explicit FakeSource( std::string                  name,
                                 std::vector<grab::EventKind> kinds = {} ) :
                name_( std::move( name ) ),
                kinds_( std::move( kinds ) )
            {
            }

            ~FakeSource() override          = default;

            FakeSource( const FakeSource& ) = delete;
            FakeSource&
            operator=( const FakeSource& ) = delete;
            FakeSource( FakeSource&& )     = delete;
            FakeSource&
            operator=( FakeSource&& ) = delete;

            [[nodiscard]]
            grab::Result<void>
            start( grab::core::Reactor& reactor,
                   grab::EventBus&      bus ) override
            {
                static_cast<void>( reactor );
                static_cast<void>( bus );

                ++start_calls_;
                if( on_start_ )
                {
                    on_start_( *this );
                }

                if( !start_result_.has_value() )
                {
                    state_ = grab::event::SourceState::Failed;
                    return std::unexpected( start_result_.error() );
                }

                state_ = grab::event::SourceState::Running;
                return {};
            }

            void
            stop() noexcept override
            {
                ++stop_calls_;
                try
                {
                    if( on_stop_ )
                    {
                        on_stop_( *this );
                    }
                }
                catch( ... )
                {
                    return;
                }

                if( state_ == grab::event::SourceState::Running )
                {
                    state_ = grab::event::SourceState::Stopped;
                }
            }

            [[nodiscard]]
            grab::event::SourceState
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

            void
            set_start_result( grab::Result<void> result )
            {
                start_result_ = std::move( result );
            }

            void
            set_state( grab::event::SourceState state ) noexcept
            {
                state_ = state;
            }

            void
            set_on_start( std::function<void( FakeSource& )> on_start )
            {
                on_start_ = std::move( on_start );
            }

            void
            set_on_stop( std::function<void( FakeSource& )> on_stop )
            {
                on_stop_ = std::move( on_stop );
            }

            [[nodiscard]]
            std::size_t
            start_calls() const noexcept
            {
                return start_calls_;
            }

            [[nodiscard]]
            std::size_t
            stop_calls() const noexcept
            {
                return stop_calls_;
            }

        private:

            std::string                        name_;
            std::vector<grab::EventKind>       kinds_;
            grab::Result<void>                 start_result_{};
            grab::event::SourceState           state_ = grab::event::SourceState::Idle;
            std::size_t                        start_calls_ = 0U;
            std::size_t                        stop_calls_  = 0U;
            std::function<void( FakeSource& )> on_start_;
            std::function<void( FakeSource& )> on_stop_;
    };

}    // namespace grab::test
