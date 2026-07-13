#pragma once

#include "grab/ids.hpp"

#include <cstdint>
#include <memory>

namespace grab::detail
{

    class IdFactory final
    {
        public:

            using Clock = std::uint64_t() noexcept;

            explicit IdFactory( Clock* clock );
            ~IdFactory();

            IdFactory( const IdFactory& ) = delete;
            IdFactory&
            operator=( const IdFactory& ) = delete;
            IdFactory( IdFactory&& )      = delete;
            IdFactory&
            operator=( IdFactory&& ) = delete;

            [[nodiscard]]
            OperationId
            next_operation_id();

            [[nodiscard]]
            SubscriptionId
            next_subscription_id();

        private:

            class Impl;
            std::unique_ptr<Impl> impl_;
    };

    [[nodiscard]]
    OperationId
    next_operation_id();

    [[nodiscard]]
    SubscriptionId
    next_subscription_id();

}    // namespace grab::detail
