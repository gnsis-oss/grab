#include "core/registry.hpp"

#include <gtest/gtest.h>

TEST( CliDoctor,
      BuiltinRegistryIsEmptyInPhase1 )
{
    const auto registry = grab::core::builtin_registry();
    EXPECT_TRUE( registry.all().empty() );
}
