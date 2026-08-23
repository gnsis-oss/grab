#include "drivers/desktop/x11/present_policy.hpp"

#include <gtest/gtest.h>
#include <string_view>

// The present policy exists because a reverse-PRIME display shows STALE
// overlay content when presents are incremental: the glass is a SinkOutput
// provider's dirty-region copy, and small late damage rects are what such
// copies drop. The decision must therefore be: full when a sink is present,
// unless the operator explicitly says otherwise — and an unrecognised
// override must never silently disable the safe default.

namespace
{

    using grab::drivers::desktop::x11::full_present_selected;
    using grab::drivers::desktop::x11::parse_present_policy;
    using grab::drivers::desktop::x11::PresentPolicy;

    constexpr std::string_view fullText        = "full";
    constexpr std::string_view incrementalText = "incremental";
    constexpr std::string_view garbageText     = "fastest";
    constexpr std::string_view emptyText;

    TEST( PresentPolicy,
          ParsesTheTwoExplicitOverrides )
    {
        static_assert( parse_present_policy( fullText ) == PresentPolicy::Full );
        static_assert( parse_present_policy( incrementalText ) ==
                       PresentPolicy::Incremental );
    }

    TEST( PresentPolicy,
          UnknownAndEmptyTextAreAuto )
    {
        static_assert( parse_present_policy( garbageText ) == PresentPolicy::Auto );
        static_assert( parse_present_policy( emptyText ) == PresentPolicy::Auto );
    }

    TEST( PresentPolicy,
          AutoFollowsTheSinkTopology )
    {
        static_assert( full_present_selected( PresentPolicy::Auto, true ) );
        static_assert( !full_present_selected( PresentPolicy::Auto, false ) );
    }

    TEST( PresentPolicy,
          ExplicitOverridesBeatTheTopology )
    {
        static_assert( full_present_selected( PresentPolicy::Full, false ) );
        static_assert( !full_present_selected( PresentPolicy::Incremental, true ) );
    }

}    // namespace
