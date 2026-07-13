#include "fake/fake_tree_source.hpp"
#include "grab/result.hpp"

#include <gtest/gtest.h>

TEST( Phase0Gate,
      StaleGenerationIsTyped )
{
    grab::testing::FakeTreeSource fake;
    const auto                    ref = fake.add_node();
    fake.bump_generation( ref );
    EXPECT_EQ( fake.resolve( ref ).error().code, grab::ErrorCode::StaleNode );
}

TEST( Phase0Gate,
      EpochBumpInvalidatesWholeTree )
{
    grab::testing::FakeTreeSource fake;
    const auto                    ref = fake.add_node();
    fake.bump_epoch();
    EXPECT_EQ( fake.resolve( ref ).error().code, grab::ErrorCode::TreeResynced );
}
