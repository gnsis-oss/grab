#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "kernel/graph/target_registry.hpp"

#include <map>
#include <memory>
#include <mutex>

namespace grab::kernel
{

    class InjectGate final
    {
        private:

            struct Lane;

        public:

            class Token final
            {
                public:

                    Token( const Token& ) = delete;
                    Token&
                    operator=( const Token& ) = delete;
                    Token( Token&& ) noexcept = default;
                    Token&
                    operator=( Token&& ) noexcept = default;
                    ~Token()                      = default;

                private:

                    friend class InjectGate;

                    explicit Token( std::shared_ptr<Lane> lane );

                    std::shared_ptr<Lane>        lane_;
                    std::unique_lock<std::mutex> lock_;
            };

            using CaptureToken              = Token;
            using InjectionToken            = Token;

            InjectGate()                    = default;
            InjectGate( const InjectGate& ) = delete;
            InjectGate&
            operator=( const InjectGate& ) = delete;
            InjectGate( InjectGate&& )     = delete;
            InjectGate&
            operator=( InjectGate&& ) = delete;
            ~InjectGate()             = default;

            [[nodiscard]]
            CaptureToken
            acquire_capture( TargetId target );

            [[nodiscard]]
            InjectionToken
            acquire_injection( TargetId target );

        private:

            struct Lane
            {
                    std::mutex mutex;
            };

            [[nodiscard]]
            std::shared_ptr<Lane>
                                                      lane_for( TargetId target );

            std::mutex                                lanes_mutex_;
            std::map<TargetId, std::shared_ptr<Lane>> lanes_;
    };

}    // namespace grab::kernel
