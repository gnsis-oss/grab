#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// Session-lifecycle helper hoisted out of the legacy core directory to preserve
// its LOC ratchet; used by Session's reactor-thread bootstrap.

#include "grab/result.hpp"

#include <future>
#include <mutex>
#include <utility>

namespace grab::kernel::lifecycle
{

    class StartupSignal
    {
        public:

            StartupSignal()                       = default;
            ~StartupSignal()                      = default;

            StartupSignal( const StartupSignal& ) = delete;
            StartupSignal&
            operator=( const StartupSignal& ) = delete;
            StartupSignal( StartupSignal&& )  = delete;
            StartupSignal&
            operator=( StartupSignal&& ) = delete;

            [[nodiscard]]
            std::future<grab::Result<void>>
            future()
            {
                return result_.get_future();
            }

            void
            report( grab::Result<void> result )
            {
                const std::scoped_lock lock( mutex_ );
                if( reported_ )
                {
                    return;
                }
                reported_ = true;
                result_.set_value( std::move( result ) );
            }

        private:

            std::mutex                       mutex_;
            bool                             reported_ = false;
            std::promise<grab::Result<void>> result_;
    };

}    // namespace grab::kernel::lifecycle
