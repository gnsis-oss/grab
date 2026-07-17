#include "grab/result.hpp"
#include "grab/trace.hpp"

#include <gtest/gtest.h>

TEST( ResultTaxonomy,
      NewCodesHaveCategoriesAndNames )
{
    EXPECT_EQ( grab::category_of( grab::ErrorCode::StaleNode ),
               grab::ErrorCategory::Target );
    EXPECT_EQ( grab::name_of( grab::ErrorCode::StaleShape ), "stale_shape" );
    EXPECT_EQ( grab::category_of( grab::ErrorCode::StaleShape ),
               grab::ErrorCategory::Target );
    EXPECT_EQ( grab::default_disposition_of( grab::ErrorCode::StaleShape ),
               grab::ErrorDisposition::RetrySame );
    EXPECT_EQ( grab::retry_class_of( grab::ErrorCode::StaleShape ),
               grab::RetryClass::ResolveOnly );
    EXPECT_EQ( grab::category_of( grab::ErrorCode::PossiblyCommitted ),
               grab::ErrorCategory::Action );
    EXPECT_EQ( grab::category_of( grab::ErrorCode::QueueGap ),
               grab::ErrorCategory::Stream );
    EXPECT_EQ( grab::name_of( grab::ErrorCode::DeadlineExceeded ), "deadline_exceeded" );
}

TEST( ResultTaxonomy,
      ErrorDefaultsToFatalDisposition )
{
    grab::Error e{
        .code       = grab::ErrorCode::StaleNode,
        .message    = "x",
        .capability = {},
        .target     = {},
        .attempts   = {},
    };
    EXPECT_EQ( e.disposition, grab::ErrorDisposition::Fatal );
    EXPECT_TRUE( e.diagnostics.empty() );
}
