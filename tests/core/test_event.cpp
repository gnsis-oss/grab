#include "grab/event.hpp"
#include "grab/event_descriptor.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstdint>
// clang-format on

namespace
{

    constexpr auto          keyDownKind            = grab::EventKind::KeyDown;
    constexpr auto          mouseMoveKind          = grab::EventKind::MouseMove;
    constexpr auto          windowFocusChangedKind = grab::EventKind::WindowFocusChanged;
    constexpr auto          a11yButtonClickedKind  = grab::EventKind::A11yButtonClicked;
    constexpr auto          appContextUpdateKind   = grab::EventKind::AppContextUpdate;
    constexpr auto          stateSnapshotKind      = grab::EventKind::StateSnapshot;
    constexpr auto          inputCategory          = grab::EventCategory::Input;
    constexpr auto          windowCategory         = grab::EventCategory::Window;
    constexpr auto          accessibilityCategory  = grab::EventCategory::Accessibility;
    constexpr auto          integrationCategory    = grab::EventCategory::Integration;
    constexpr auto          stateCategory          = grab::EventCategory::State;
    constexpr double        timestamp              = 1.25;
    constexpr std::uint64_t sequence               = 7U;

    [[nodiscard]]
    grab::Event
    make_event( grab::EventKind     kind,
                grab::EventCategory category )
    {
        return grab::Event{
            .timestamp = timestamp,
            .sequence  = sequence,
            .kind      = kind,
            .category  = category,
            .payload   = grab::Payload{ grab::InputKey{} },
        };
    }

}    // namespace

TEST( EventModel,
      CategoryOfMapsRepresentativeKindsAtCompileTime )
{
    static_assert( grab::category_of( keyDownKind ) == inputCategory );
    static_assert( grab::category_of( windowFocusChangedKind ) == windowCategory );
    static_assert( grab::category_of( a11yButtonClickedKind ) == accessibilityCategory );
    static_assert( grab::category_of( appContextUpdateKind ) == integrationCategory );
    static_assert( grab::category_of( stateSnapshotKind ) == stateCategory );
}

TEST( EventFilter,
      AllWildcardMatchesEverything )
{
    const grab::EventFilter filter;
    const grab::Event       event = make_event( keyDownKind, inputCategory );

    EXPECT_TRUE( filter.matches( event ) );
}

TEST( EventFilter,
      KindOnlyFilterMatchesSelectedKind )
{
    const grab::EventFilter filter{
        .kinds      = { mouseMoveKind },
        .categories = {},
    };
    const grab::Event matching_event = make_event( mouseMoveKind, inputCategory );
    const grab::Event other_event = make_event( windowFocusChangedKind, windowCategory );

    EXPECT_TRUE( filter.matches( matching_event ) );
    EXPECT_FALSE( filter.matches( other_event ) );
}

TEST( EventFilter,
      CategoryOnlyFilterMatchesSelectedCategory )
{
    const grab::EventFilter filter{
        .kinds      = {},
        .categories = { windowCategory },
    };
    const grab::Event matching_event =
        make_event( windowFocusChangedKind, windowCategory );
    const grab::Event other_event = make_event( mouseMoveKind, inputCategory );

    EXPECT_TRUE( filter.matches( matching_event ) );
    EXPECT_FALSE( filter.matches( other_event ) );
}

TEST( EventFilter,
      BothListsMustMatch )
{
    const grab::EventFilter filter{
        .kinds      = { stateSnapshotKind },
        .categories = { stateCategory },
    };
    const grab::Event matching_event = make_event( stateSnapshotKind, stateCategory );
    const grab::Event wrong_category_event =
        make_event( stateSnapshotKind, windowCategory );
    const grab::Event wrong_kind_event = make_event( keyDownKind, stateCategory );

    EXPECT_TRUE( filter.matches( matching_event ) );
    EXPECT_FALSE( filter.matches( wrong_category_event ) );
    EXPECT_FALSE( filter.matches( wrong_kind_event ) );
}

TEST( EventFilter,
      NonMatchingEventIsRejected )
{
    const grab::EventFilter filter{
        .kinds      = { stateSnapshotKind },
        .categories = { stateCategory },
    };
    const grab::Event event = make_event( appContextUpdateKind, integrationCategory );

    EXPECT_FALSE( filter.matches( event ) );
}
