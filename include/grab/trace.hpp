#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/enum_table.hpp"
#include "grab/ids.hpp"
#include "grab/space.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace grab
{

    enum class ErrorCode : std::uint16_t;

    enum class RetryClass : std::uint8_t
    {
        Never,
        ResolveOnly,
        Idempotent,
        Compensated,
    };

    enum class ErrorDisposition : std::uint8_t
    {
        RetrySame,
        FallbackNext,
        Fatal,
    };

    enum class CommitStatus : std::uint8_t
    {
        FailedBeforeCommit,
        PossiblyCommitted,
        Committed,
        Verified,
    };

    enum class NeutralizationOutcome : std::uint8_t
    {
        NotAttempted,
        NothingHeld,
        Released,
        Failed,
    };

    struct DiagnosticEntry
    {
            std::chrono::steady_clock::time_point
                        at{};    // NOLINT(readability-redundant-member-init)
            std::string message;
    };

    struct RouteAttempt
    {
            std::string route;
            bool        selected{};
            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
            ErrorCode   rejection{};
            std::string detail;
    };

    struct BarrierOutcome
    {
            std::string barrier;
            bool        satisfied{};
            bool        timed_out{};
    };

    struct Receipt
    {
            OperationId                  operation{};
            std::string                  locator;
            std::uint64_t                snapshot_revision{};
            CommitStatus                 commit{ CommitStatus::FailedBeforeCommit };
            std::vector<RouteAttempt>    routes;
            std::vector<BarrierOutcome>  barriers;
            std::vector<TransformRecord> transforms;
            std::vector<FrameRef>        frames;
            bool                         forced{};
            bool                         fallback_used{};
            NeutralizationOutcome        neutralization{
                NeutralizationOutcome::NotAttempted,
            };
            RetryClass                   retry_class{ RetryClass::Never };
            std::uint32_t                resolve_retries{};
            std::vector<DiagnosticEntry> log;
    };

    namespace detail
    {

        inline constexpr std::size_t retryClassCount  = 4U;
        inline constexpr auto        retry_class_name = EnumTable{
            std::to_array( {
                enum_entry( RetryClass::Never, "never" ),
                enum_entry( RetryClass::ResolveOnly, "resolve_only" ),
                enum_entry( RetryClass::Idempotent, "idempotent" ),
                enum_entry( RetryClass::Compensated, "compensated" ),
            } ),
        };
        static_assert( enum_table_has_count( retry_class_name,
                                             retryClassCount ) );

        inline constexpr std::size_t errorDispositionCount  = 3U;
        inline constexpr auto        error_disposition_name = EnumTable{
            std::to_array( {
                enum_entry( ErrorDisposition::RetrySame, "retry_same" ),
                enum_entry( ErrorDisposition::FallbackNext, "fallback_next" ),
                enum_entry( ErrorDisposition::Fatal, "fatal" ),
            } ),
        };
        static_assert( enum_table_has_count( error_disposition_name,
                                             errorDispositionCount ) );

        inline constexpr std::size_t commitStatusCount  = 4U;
        inline constexpr auto        commit_status_name = EnumTable{
            std::to_array( {
                enum_entry( CommitStatus::FailedBeforeCommit, "failed_before_commit" ),
                enum_entry( CommitStatus::PossiblyCommitted, "possibly_committed" ),
                enum_entry( CommitStatus::Committed, "committed" ),
                enum_entry( CommitStatus::Verified, "verified" ),
            } ),
        };
        static_assert( enum_table_has_count( commit_status_name,
                                             commitStatusCount ) );

    }    // namespace detail

}    // namespace grab
