#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/ids.hpp"
#include "grab/result.hpp"
#include "grab/trace.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace grab
{

    struct Deadline
    {
            std::chrono::steady_clock::time_point at{
                std::chrono::steady_clock::time_point::max(),
            };

            [[nodiscard]]
            static Deadline
            after( std::chrono::nanoseconds budget );

            [[nodiscard]]
            static Deadline
            unbounded();

            [[nodiscard]]
            std::chrono::nanoseconds
            remaining() const;

            [[nodiscard]]
            bool
            expired() const;
    };

    class DiagnosticLog
    {
        public:

            // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers)
            explicit DiagnosticLog( std::size_t capacity = 256U );
            // NOLINTEND(cppcoreguidelines-avoid-magic-numbers)

            void
            note( std::string message );

            [[nodiscard]]
            std::vector<DiagnosticEntry>
            snapshot() const;

            [[nodiscard]]
            std::size_t
            dropped() const;

        private:

            std::vector<DiagnosticEntry> ring_;
            std::size_t                  capacity_;
            std::size_t                  next_{};
            std::size_t                  dropped_{};
    };

    struct OperationContext
    {
            Deadline        deadline{};
            std::stop_token stop{};    // NOLINT(readability-redundant-member-init)
            OperationId     operation{};
            std::optional<OperationId>
                causal_parent{};       // NOLINT(readability-redundant-member-init)
            DiagnosticLog* log{ nullptr };

            [[nodiscard]]
            Result<void>
            check() const;

            void
            note( std::string message ) const;

            [[nodiscard]]
            OperationContext
            nested() const;
    };

}    // namespace grab
