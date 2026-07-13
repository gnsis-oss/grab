#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/context.hpp"
#include "grab/result.hpp"
#include "grab/ui.hpp"
#include "spi/event_source.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace grab::kernel::action
{

    struct WaitBackoff
    {
            std::chrono::nanoseconds initial{ std::chrono::milliseconds{ 1 } };
            std::chrono::nanoseconds maximum{ std::chrono::milliseconds{ 50 } };
    };

    struct WaitParams
    {
            Deadline    deadline{ Deadline::unbounded() };
            WaitBackoff backoff{};
    };

    struct PredicateObservation
    {
            bool        satisfied{};
            std::string detail;
    };

    struct NamedPredicate
    {
            std::string                                   name;
            std::function<Result<PredicateObservation>()> observe;
    };

    struct NodeObservation
    {
            bool          present{};
            std::uint32_t states{};
            std::string   detail;
    };

    using NodeObserver = std::function<Result<NodeObservation>()>;

    [[nodiscard]]
    NamedPredicate
    node_present( NodeObserver observer );

    [[nodiscard]]
    NamedPredicate
    enabled( NodeObserver observer );

    [[nodiscard]]
    NamedPredicate
    state_stable( NodeObserver observer,
                  std::size_t  required_observations );

    [[nodiscard]]
    NamedPredicate
    all_of( std::string                 name,
            std::vector<NamedPredicate> predicates );

    class WaitEngine
    {
        public:

            explicit WaitEngine( OperationContext context );

            [[nodiscard]]
            Result<void>
            wait( NamedPredicate&   predicate,
                  const WaitParams& params,
                  spi::EventSource& event_source ) const;

        private:

            OperationContext context_;
    };

}    // namespace grab::kernel::action
