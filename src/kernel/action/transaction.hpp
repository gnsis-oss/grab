#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/context.hpp"
#include "grab/interaction.hpp"
#include "grab/result.hpp"
#include "grab/trace.hpp"
#include "spi/runtime.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace grab::kernel::action
{

    struct TransactionOutcome
    {
            Receipt              receipt;
            std::optional<Error> error;

            [[nodiscard]]
            bool
            succeeded() const noexcept
            {
                return !error.has_value();
            }
    };

    using MappingRefreshHook =
        std::function<Result<std::vector<TransformRecord>>( const Match&,
                                                            const OperationContext& )>;

    class Transaction
    {
        public:

            Transaction( spi::Runtime&      runtime,
                         std::uint32_t      tree,
                         MappingRefreshHook mapping_refresh = {} );

            [[nodiscard]]
            TransactionOutcome
            perform( const Action&        action,
                     const ActionOptions& options = {} ) const;

        private:

            spi::Runtime*      runtime_{};
            std::uint32_t      tree_{};
            MappingRefreshHook mapping_refresh_;
    };

}    // namespace grab::kernel::action
