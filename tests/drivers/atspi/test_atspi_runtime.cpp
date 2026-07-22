#include "drivers/semantic/atspi/atspi_runtime.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstdint>
#include <string_view>
// clang-format on

namespace
{

    constexpr std::uint32_t initialGeneration = 1U;

}    // namespace

TEST( AtspiRuntime,
      ExposesIdentityAndSemanticRoutesBeforeStart )
{
    const grab::drivers::semantic::atspi::AtspiRuntime runtime;

    EXPECT_EQ( runtime.name(), std::string_view{ "atspi" } );
    EXPECT_EQ( runtime.generation(), initialGeneration );
    EXPECT_FALSE( runtime.routes().empty() );
}

// A live accessibility bus is intentionally unavailable in this test environment.
TEST( AtspiRuntime,
      DISABLED_StartRequiresLiveBus )
{
    GTEST_SKIP() << "Requires a live AT-SPI accessibility bus";
}
