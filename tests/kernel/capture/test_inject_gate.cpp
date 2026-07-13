#include "kernel/capture/inject_gate.hpp"
#include "kernel/graph/target_registry.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <future>
#include <optional>
// clang-format on

using namespace std::chrono_literals;

TEST( InjectGate,
      InjectionWaitsForCaptureOfSameTarget )
{
    grab::kernel::InjectGate         gate;
    constexpr grab::kernel::TargetId target{ 42U };
    auto               capture = std::optional{ gate.acquire_capture( target ) };

    std::promise<void> attempting;
    auto               attempted = attempting.get_future();
    auto               injection =
        std::async( std::launch::async,
                    [&gate, target, attempting = std::move( attempting )]() mutable
                    {
                        attempting.set_value();
                        const auto token = gate.acquire_injection( target );
                        static_cast<void>( token );
                    } );

    attempted.wait();
    EXPECT_EQ( injection.wait_for( 50ms ), std::future_status::timeout );
    capture.reset();
    EXPECT_EQ( injection.wait_for( 1s ), std::future_status::ready );
    injection.get();
}
