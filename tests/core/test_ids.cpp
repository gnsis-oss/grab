#include "core/id_factory.hpp"
#include "grab/ids.hpp"

// clang-format and llvm-include-order disagree about these include categories.
// NOLINTBEGIN(llvm-include-order)
#include <cstdint>
#include <gtest/gtest.h>
// NOLINTEND(llvm-include-order)

namespace
{

    constexpr std::uint64_t fixedUnixMilliseconds = 1'700'000'000'000ULL;
    constexpr std::uint32_t windowXid             = 42U;

    [[nodiscard]]
    std::uint64_t
    fixed_clock() noexcept
    {
        return fixedUnixMilliseconds;
    }

}    // namespace

TEST( Ids,
      DefaultUuidIsNil )
{
    grab::Uuid u;
    EXPECT_TRUE( u.is_nil() );
    EXPECT_EQ( u.to_string(), "00000000-0000-0000-0000-000000000000" );
}

TEST( Ids,
      OperationIdsAreUniqueAndTimeOrdered )
{
    const auto a = grab::detail::next_operation_id();
    const auto b = grab::detail::next_operation_id();
    EXPECT_NE( a, b );
    EXPECT_LT( a, b );    // v7: millisecond-prefixed, monotonic within factory
}

TEST( Ids,
      FakeClockOrdersIdsWithinSameMillisecond )
{
    grab::detail::IdFactory factory{ fixed_clock };

    const auto              a = factory.next_operation_id();
    const auto              b = factory.next_operation_id();

    EXPECT_LT( a, b );
}

TEST( Ids,
      WindowRefEqualityIncludesGeneration )
{
    grab::WindowRef w1{ .display_generation = { 1 }, .xid = windowXid };
    grab::WindowRef w2{ .display_generation = { 2 }, .xid = windowXid };
    EXPECT_NE( w1, w2 );    // same XID, different generation -> different ref
}
