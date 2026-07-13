#include "grab/result.hpp"
#include "grab/trace.hpp"

#include <gtest/gtest.h>

TEST( ResultTaxonomy,
      NewCodesHaveCategoriesAndNames )
{
    EXPECT_EQ( grab::category_of( grab::ErrorCode::StaleNode ),
               grab::ErrorCategory::Target );
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
