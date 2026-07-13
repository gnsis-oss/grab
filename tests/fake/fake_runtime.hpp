#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "fake/fake_tree_source.hpp"
#include "grab/context.hpp"
#include "grab/ids.hpp"
#include "grab/result.hpp"
#include "grab/ui.hpp"
#include "spi/event_source.hpp"
#include "spi/route.hpp"
#include "spi/runtime.hpp"
#include "spi/tree_source.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab::testing
{

    class FakeEventSource final : public spi::EventSource
    {
        public:

            [[nodiscard]]
            Result<void>
            enable( const spi::EventSpec& spec ) override
            {
                ++demand_[spec.name];
                return {};
            }

            [[nodiscard]]
            Result<void>
            disable( const spi::EventSpec& spec ) override
            {
                const auto found = demand_.find( spec.name );
                if( found == demand_.end() )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "event demand is not enabled" );
                }

                if( found->second == firstDemand )
                {
                    demand_.erase( found );
                }
                else
                {
                    --found->second;
                }
                return {};
            }

            [[nodiscard]]
            Result<void>
            wait_for_event(
                [[maybe_unused]] const spi::EventSpec&    spec,
                const OperationContext&                   context,
                [[maybe_unused]] std::chrono::nanoseconds maximum_wait
            ) override
            {
                ++wait_count_;
                const auto checked = context.check();
                if( !checked.has_value() )
                {
                    return std::unexpected( checked.error() );
                }
                if( wakeups_.empty() )
                {
                    return fail( ErrorCode::DeadlineExceeded,
                                 "fake event source has no scripted wakeup" );
                }
                auto wakeup = std::move( wakeups_.front() );
                wakeups_.pop_front();
                wakeup();
                return {};
            }

            void
            push_wakeup( std::function<void()> wakeup )
            {
                wakeups_.push_back( std::move( wakeup ) );
            }

            [[nodiscard]]
            std::size_t
            wait_count() const noexcept
            {
                return wait_count_;
            }

            [[nodiscard]]
            std::size_t
            demand_count( const spi::EventSpec& spec ) const noexcept
            {
                const auto found = demand_.find( spec.name );
                return found == demand_.end() ? noDemand : found->second;
            }

            void
            clear() noexcept
            {
                demand_.clear();
                wakeups_.clear();
                wait_count_ = 0U;
            }

        private:

            static constexpr std::size_t       noDemand    = 0U;
            static constexpr std::size_t       firstDemand = 1U;

            std::map<std::string, std::size_t> demand_;
            std::deque<std::function<void()>>  wakeups_;
            std::size_t                        wait_count_{};
    };

    class FakeActionRoute;

    class FakeRouteReservation final : public spi::RouteReservation
    {
        public:

            explicit FakeRouteReservation( FakeActionRoute& route );

            [[nodiscard]]
            std::span<const std::string_view>
            barriers() const noexcept override;

            [[nodiscard]]
            Result<void>
            arm_barrier( std::string_view        barrier,
                         const OperationContext& context ) override;

            [[nodiscard]]
            Result<void>
            commit( const OperationContext& context ) override;

            [[nodiscard]]
            Result<std::vector<BarrierOutcome>>
            settle( const OperationContext& context ) override;

            [[nodiscard]]
            Result<void>
            verify( const OperationContext& context ) override;

        private:

            FakeActionRoute*              route_{};
            std::vector<std::string_view> barriers_;
    };

    class FakeActionRoute final : public spi::ActionRoute
    {
        public:

            FakeActionRoute( std::string               name,
                             spi::RouteKind            kind,
                             std::vector<std::string>& action_log ) :
                name_{ std::move( name ) },
                kind_{ kind },
                action_log_{ &action_log }
            {
            }

            [[nodiscard]]
            std::string_view
            name() const noexcept
            {
                return name_;
            }

            [[nodiscard]]
            spi::RouteKind
            kind() const noexcept
            {
                return kind_;
            }

            void
            add_barrier( std::string name,
                         bool        satisfied,
                         bool        timed_out )
            {
                barriers_.push_back( BarrierOutcome{
                    .barrier   = std::move( name ),
                    .satisfied = satisfied,
                    .timed_out = timed_out,
                } );
            }

            void
            set_reserve_error( ErrorCode   code,
                               std::string message )
            {
                reserve_error_ = error( code, std::move( message ) );
            }

            void
            set_arm_error( ErrorCode   code,
                           std::string message )
            {
                arm_error_ = error( code, std::move( message ) );
            }

            void
            set_commit_error( ErrorCode   code,
                              std::string message )
            {
                commit_error_ = error( code, std::move( message ) );
            }

            void
            set_settle_error( ErrorCode   code,
                              std::string message )
            {
                settle_error_ = error( code, std::move( message ) );
            }

            void
            set_verify_error( ErrorCode   code,
                              std::string message )
            {
                verify_error_ = error( code, std::move( message ) );
            }

            [[nodiscard]]
            std::size_t
            reserve_count() const noexcept
            {
                return reserve_count_;
            }

            [[nodiscard]]
            std::size_t
            arm_count() const noexcept
            {
                return arm_count_;
            }

            [[nodiscard]]
            std::size_t
            commit_count() const noexcept
            {
                return commit_count_;
            }

            [[nodiscard]]
            Result<std::unique_ptr<spi::RouteReservation>>
            reserve( [[maybe_unused]] const spi::ActionRequest& action,
                     const OperationContext&                    context ) override
            {
                ++reserve_count_;
                action_log_->push_back( "reserve:" + name_ );
                const auto checked = context.check();
                if( !checked.has_value() )
                {
                    return std::unexpected( checked.error() );
                }
                if( reserve_error_.has_value() )
                {
                    return std::unexpected( *reserve_error_ );
                }
                return std::make_unique<FakeRouteReservation>( *this );
            }

        private:

            [[nodiscard]]
            static Error
            error( ErrorCode   code,
                   std::string message )
            {
                return Error{
                    .code        = code,
                    .message     = std::move( message ),
                    .capability  = {},
                    .target      = {},
                    .attempts    = {},
                    .disposition = ErrorDisposition::Fatal,
                    .diagnostics = {},
                };
            }

            std::string                 name_;
            spi::RouteKind              kind_;
            std::vector<BarrierOutcome> barriers_;
            std::optional<Error>        reserve_error_;
            std::optional<Error>        arm_error_;
            std::optional<Error>        commit_error_;
            std::optional<Error>        settle_error_;
            std::optional<Error>        verify_error_;
            std::size_t                 reserve_count_{};
            std::size_t                 arm_count_{};
            std::size_t                 commit_count_{};
            std::vector<std::string>*   action_log_{};

            friend class FakeRouteReservation;
    };

    inline FakeRouteReservation::FakeRouteReservation( FakeActionRoute& route ) :
        route_{ &route }
    {
        barriers_.reserve( route.barriers_.size() );
        for( const auto& barrier : route.barriers_ )
        {
            barriers_.push_back( barrier.barrier );
        }
    }

    inline std::span<const std::string_view>
    FakeRouteReservation::barriers() const noexcept
    {
        return barriers_;
    }

    inline Result<void>
    FakeRouteReservation::arm_barrier( std::string_view        barrier,
                                       const OperationContext& context )
    {
        ++route_->arm_count_;
        route_->action_log_->push_back( "arm:" + std::string{ barrier } );
        const auto checked = context.check();
        if( !checked.has_value() )
        {
            return std::unexpected( checked.error() );
        }
        if( route_->arm_error_.has_value() )
        {
            return std::unexpected( *route_->arm_error_ );
        }
        return {};
    }

    inline Result<void>
    FakeRouteReservation::commit( const OperationContext& context )
    {
        ++route_->commit_count_;
        route_->action_log_->push_back( "commit" );
        const auto checked = context.check();
        if( !checked.has_value() )
        {
            return std::unexpected( checked.error() );
        }
        if( route_->commit_error_.has_value() )
        {
            return std::unexpected( *route_->commit_error_ );
        }
        return {};
    }

    inline Result<std::vector<BarrierOutcome>>
    FakeRouteReservation::settle( const OperationContext& context )
    {
        route_->action_log_->push_back( "settle" );
        const auto checked = context.check();
        if( !checked.has_value() )
        {
            return std::unexpected( checked.error() );
        }
        if( route_->settle_error_.has_value() )
        {
            return std::unexpected( *route_->settle_error_ );
        }
        return route_->barriers_;
    }

    inline Result<void>
    FakeRouteReservation::verify( const OperationContext& context )
    {
        route_->action_log_->push_back( "verify" );
        const auto checked = context.check();
        if( !checked.has_value() )
        {
            return std::unexpected( checked.error() );
        }
        if( route_->verify_error_.has_value() )
        {
            return std::unexpected( *route_->verify_error_ );
        }
        return {};
    }

    class FakeInputSeat final : public spi::InputSeat
    {
        public:

            explicit FakeInputSeat( std::vector<std::string>& action_log ) :
                action_log_{ &action_log }
            {
            }

            [[nodiscard]]
            Result<NeutralizationOutcome>
            neutralize( [[maybe_unused]] const OperationContext& context ) override
            {
                ++neutralize_count_;
                action_log_->push_back( "neutralize" );
                if( error_.has_value() )
                {
                    return std::unexpected( *error_ );
                }
                return outcome_;
            }

            void
            set_outcome( NeutralizationOutcome outcome ) noexcept
            {
                outcome_ = outcome;
                error_.reset();
            }

            void
            set_error( std::string message )
            {
                error_ = Error{
                    .code        = ErrorCode::NeutralizationFailed,
                    .message     = std::move( message ),
                    .capability  = {},
                    .target      = {},
                    .attempts    = {},
                    .disposition = ErrorDisposition::Fatal,
                    .diagnostics = {},
                };
            }

            [[nodiscard]]
            std::size_t
            neutralize_count() const noexcept
            {
                return neutralize_count_;
            }

        private:

            NeutralizationOutcome     outcome_{ NeutralizationOutcome::NothingHeld };
            std::optional<Error>      error_;
            std::size_t               neutralize_count_{};
            std::vector<std::string>* action_log_{};
    };

    class FakeRuntime final : public spi::Runtime
    {
        public:

            FakeRuntime() :
                tree_source_( runtime_id_ ),
                seat_{ action_log_ }
            {
            }

            [[nodiscard]]
            std::string_view
            name() const override
            {
                return "fake";
            }

            [[nodiscard]]
            std::uint32_t
            generation() const override
            {
                return generation_;
            }

            [[nodiscard]]
            RuntimeId
            runtime_id() const noexcept
            {
                return runtime_id_;
            }

            [[nodiscard]]
            Result<void>
            start( const OperationContext& context ) override
            {
                const auto contextResult = context.check();
                if( !contextResult.has_value() )
                {
                    return std::unexpected( contextResult.error() );
                }
                started_ = true;
                return {};
            }

            [[nodiscard]]
            Result<void>
            stop() override
            {
                started_ = false;
                event_source_.clear();
                return {};
            }

            void
            restart() noexcept
            {
                ++generation_;
                ++runtime_id_.value;
                tree_source_.restart( runtime_id_ );
                event_source_.clear();
            }

            [[nodiscard]]
            FakeTreeSource*
            tree_source() override
            {
                return &tree_source_;
            }

            [[nodiscard]]
            FakeEventSource*
            event_source() override
            {
                return &event_source_;
            }

            [[nodiscard]]
            std::span<const spi::RouteDescriptor>
            routes() const override
            {
                return route_descriptors_;
            }

            [[nodiscard]]
            spi::ActionRoute*
            action_route( std::size_t index ) override
            {
                return index < action_routes_.size() ? action_routes_.at( index ).get()
                                                     : nullptr;
            }

            [[nodiscard]]
            FakeInputSeat*
            input_seat() override
            {
                return &seat_;
            }

            FakeActionRoute&
            add_route( std::string    name,
                       spi::RouteKind kind )
            {
                auto        route = std::make_unique<FakeActionRoute>( std::move( name ),
                                                                       kind,
                                                                       action_log_ );
                auto* const route_pointer = route.get();
                action_routes_.push_back( std::move( route ) );
                route_descriptors_.push_back( spi::RouteDescriptor{
                    .name          = route_pointer->name(),
                    .kind          = route_pointer->kind(),
                    .fidelity      = spi::RouteFidelity::Exact,
                    .latency_class = spi::RouteLatencyClass::Immediate,
                    .constraints   = {},
                } );
                return *route_pointer;
            }

            [[nodiscard]]
            FakeInputSeat&
            seat() noexcept
            {
                return seat_;
            }

            [[nodiscard]]
            const std::vector<std::string>&
            action_log() const noexcept
            {
                return action_log_;
            }

            [[nodiscard]]
            WidgetRef
            add_node()
            {
                return tree_source_.add_node();
            }

            void
            bump_epoch() noexcept
            {
                tree_source_.bump_epoch();
            }

            void
            bump_generation( const WidgetRef& ref ) noexcept
            {
                tree_source_.bump_generation( ref );
            }

            [[nodiscard]]
            Result<WidgetRef>
            resolve( const WidgetRef& ref ) const
            {
                return tree_source_.resolve( ref );
            }

            void
            inject_snapshot( UiSnapshot snapshot )
            {
                tree_source_.inject_snapshot( std::move( snapshot ) );
            }

            void
            inject_delta( spi::UiDelta delta )
            {
                tree_source_.inject_delta( std::move( delta ) );
            }

            void
            inject_overflow( std::uint64_t dropped )
            {
                tree_source_.inject_overflow( dropped );
            }

            void
            inject_partial_commit( UiSnapshot authoritative_after )
            {
                tree_source_.inject_partial_commit( std::move( authoritative_after ) );
            }

        private:

            static constexpr std::uint32_t                firstGeneration = 1U;

            std::uint32_t                                 generation_{ firstGeneration };
            RuntimeId                                     runtime_id_{ firstGeneration };
            bool                                          started_{};
            FakeTreeSource                                tree_source_;
            FakeEventSource                               event_source_;
            std::vector<std::string>                      action_log_;
            FakeInputSeat                                 seat_;
            std::vector<std::unique_ptr<FakeActionRoute>> action_routes_;
            std::vector<spi::RouteDescriptor>             route_descriptors_;
    };

}    // namespace grab::testing
