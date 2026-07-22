#include "grab/origin.hpp"
#include "grab/trace.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <string_view>
// clang-format on

namespace
{

    template<typename Table,
             typename Enum>
    void
    expect_round_trip( const Table&     names,
                       Enum             value,
                       std::string_view expected )
    {
        EXPECT_EQ( names.text_of( value, {} ), expected );

        const auto parsed = names.value_of( expected );
        ASSERT_TRUE( parsed.has_value() );
        EXPECT_EQ( *parsed, value );
    }

}    // namespace

TEST( TraceEnums,
      RetryClassNamesAreStable )
{
    expect_round_trip( grab::detail::retry_class_name,
                       grab::RetryClass::Never,
                       "never" );
    expect_round_trip( grab::detail::retry_class_name,
                       grab::RetryClass::ResolveOnly,
                       "resolve_only" );
    expect_round_trip( grab::detail::retry_class_name,
                       grab::RetryClass::Idempotent,
                       "idempotent" );
    expect_round_trip( grab::detail::retry_class_name,
                       grab::RetryClass::Compensated,
                       "compensated" );
}

TEST( TraceEnums,
      ErrorDispositionNamesAreStable )
{
    expect_round_trip( grab::detail::error_disposition_name,
                       grab::ErrorDisposition::RetrySame,
                       "retry_same" );
    expect_round_trip( grab::detail::error_disposition_name,
                       grab::ErrorDisposition::FallbackNext,
                       "fallback_next" );
    expect_round_trip( grab::detail::error_disposition_name,
                       grab::ErrorDisposition::Fatal,
                       "fatal" );
}

TEST( TraceEnums,
      CommitStatusNamesAreStable )
{
    expect_round_trip( grab::detail::commit_status_name,
                       grab::CommitStatus::FailedBeforeCommit,
                       "failed_before_commit" );
    expect_round_trip( grab::detail::commit_status_name,
                       grab::CommitStatus::PossiblyCommitted,
                       "possibly_committed" );
    expect_round_trip( grab::detail::commit_status_name,
                       grab::CommitStatus::Committed,
                       "committed" );
    expect_round_trip( grab::detail::commit_status_name,
                       grab::CommitStatus::Verified,
                       "verified" );
}

TEST( TraceEnums,
      EventOriginNamesAreStable )
{
    expect_round_trip( grab::detail::event_origin_name,
                       grab::EventOrigin::Physical,
                       "physical" );
    expect_round_trip( grab::detail::event_origin_name,
                       grab::EventOrigin::InjectedSelf,
                       "injected_self" );
    expect_round_trip( grab::detail::event_origin_name,
                       grab::EventOrigin::InjectedOther,
                       "injected_other" );
    expect_round_trip( grab::detail::event_origin_name,
                       grab::EventOrigin::Unknown,
                       "unknown" );
}
