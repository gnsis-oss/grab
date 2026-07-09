#include "grab/event.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstdint>
// clang-format on

namespace
{

    constexpr auto   kKeyDownKind            = grab::EventKind::key_down;
    constexpr auto   kMouseMoveKind          = grab::EventKind::mouse_move;
    constexpr auto   kWindowFocusChangedKind = grab::EventKind::window_focus_changed;
    constexpr auto   kA11yButtonClickedKind  = grab::EventKind::a11y_button_clicked;
    constexpr auto   kAppContextUpdateKind   = grab::EventKind::app_context_update;
    constexpr auto   kBrowserTabSwitchedKind = grab::EventKind::browser_tab_switched;
    constexpr auto   kStateSnapshotKind      = grab::EventKind::state_snapshot;
    constexpr auto   kInputCategory          = grab::EventCategory::input;
    constexpr auto   kWindowCategory         = grab::EventCategory::window;
    constexpr auto   kAccessibilityCategory  = grab::EventCategory::accessibility;
    constexpr auto   kIntegrationCategory    = grab::EventCategory::integration;
    constexpr auto   kBrowserCategory        = grab::EventCategory::browser;
    constexpr auto   kStateCategory          = grab::EventCategory::state;
    constexpr double kTimestamp              = 1.25;
    constexpr std::uint64_t kSequence        = 7U;

    [[nodiscard]]
    grab::Event
    make_event( grab::EventKind     kind,
                grab::EventCategory category )
    {
        return grab::Event{
            .timestamp = kTimestamp,
            .sequence  = kSequence,
            .kind      = kind,
            .category  = category,
            .payload   = grab::Payload{ grab::InputKey{} },
        };
    }

}    // namespace

TEST( EventModel,
      CategoryOfMapsRepresentativeKindsAtCompileTime )
{
    static_assert( grab::category_of( kKeyDownKind ) == kInputCategory );
    static_assert( grab::category_of( kWindowFocusChangedKind ) == kWindowCategory );
    static_assert( grab::category_of( kA11yButtonClickedKind ) ==
                   kAccessibilityCategory );
    static_assert( grab::category_of( kAppContextUpdateKind ) == kIntegrationCategory );
    static_assert( grab::category_of( kBrowserTabSwitchedKind ) == kBrowserCategory );
    static_assert( grab::category_of( kStateSnapshotKind ) == kStateCategory );
}

TEST( EventFilter,
      AllWildcardMatchesEverything )
{
    const grab::EventFilter filter;
    const grab::Event       event = make_event( kKeyDownKind, kInputCategory );

    EXPECT_TRUE( filter.matches( event ) );
}

TEST( EventFilter,
      KindOnlyFilterMatchesSelectedKind )
{
    const grab::EventFilter filter{
        .kinds      = { kMouseMoveKind },
        .categories = {},
    };
    const grab::Event matching_event = make_event( kMouseMoveKind, kInputCategory );
    const grab::Event other_event =
        make_event( kWindowFocusChangedKind, kWindowCategory );

    EXPECT_TRUE( filter.matches( matching_event ) );
    EXPECT_FALSE( filter.matches( other_event ) );
}

TEST( EventFilter,
      CategoryOnlyFilterMatchesSelectedCategory )
{
    const grab::EventFilter filter{
        .kinds      = {},
        .categories = { kWindowCategory },
    };
    const grab::Event matching_event =
        make_event( kWindowFocusChangedKind, kWindowCategory );
    const grab::Event other_event = make_event( kMouseMoveKind, kInputCategory );

    EXPECT_TRUE( filter.matches( matching_event ) );
    EXPECT_FALSE( filter.matches( other_event ) );
}

TEST( EventFilter,
      BothListsMustMatch )
{
    const grab::EventFilter filter{
        .kinds      = { kBrowserTabSwitchedKind },
        .categories = { kBrowserCategory },
    };
    const grab::Event matching_event =
        make_event( kBrowserTabSwitchedKind, kBrowserCategory );
    const grab::Event wrong_category_event =
        make_event( kBrowserTabSwitchedKind, kWindowCategory );
    const grab::Event wrong_kind_event = make_event( kKeyDownKind, kBrowserCategory );

    EXPECT_TRUE( filter.matches( matching_event ) );
    EXPECT_FALSE( filter.matches( wrong_category_event ) );
    EXPECT_FALSE( filter.matches( wrong_kind_event ) );
}

TEST( EventFilter,
      NonMatchingEventIsRejected )
{
    const grab::EventFilter filter{
        .kinds      = { kStateSnapshotKind },
        .categories = { kStateCategory },
    };
    const grab::Event event = make_event( kAppContextUpdateKind, kIntegrationCategory );

    EXPECT_FALSE( filter.matches( event ) );
}
