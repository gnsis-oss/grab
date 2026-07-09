#include "grab/window.hpp"
#include "input/fake_input_sink.hpp"
#include "input/gesture.hpp"
#include "input/input_sink.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>
// clang-format on

namespace
{

    namespace input                            = grab::input;
    namespace test                             = grab::test;

    constexpr bool             press           = true;
    constexpr bool             release         = false;
    constexpr bool             clear_modifiers = true;
    constexpr bool             keep_modifiers  = false;
    constexpr std::uint8_t     left_button     = 1U;

    constexpr grab::WindowRect click_rect{
        .x      = 10,
        .y      = 20,
        .width  = 101U,
        .height = 51U,
    };
    constexpr double       click_fx = 0.5;
    constexpr double       click_fy = 0.5;
    constexpr input::Point click_point{
        .x = 60,
        .y = 46,
    };

    constexpr grab::WindowRect menu_rect{
        .x      = 100,
        .y      = 200,
        .width  = 1'000U,
        .height = 1'000U,
    };
    constexpr double        menu_header_fx         = 0.250;
    constexpr double        menu_item_fx           = 0.330;
    constexpr double        menu_first_item_fy     = 0.0475;
    constexpr double        menu_item_spacing_fy   = 0.0266;
    constexpr std::int32_t  menu_index             = 2;
    constexpr double        menu_index_two_item_fy = 0.1007;
    constexpr std::uint32_t menu_open_wait_millis  = 450U;
    constexpr std::uint32_t menu_item_wait_millis  = 900U;
    constexpr input::Point  menu_header_point{
        .x = 350,
        .y = 215,
    };
    constexpr input::Point menu_item_point{
        .x = 430,
        .y = 301,
    };

    constexpr grab::WindowRect drag_rect{
        .x      = 1'000,
        .y      = 1'000,
        .width  = 1'000U,
        .height = 1'000U,
    };
    constexpr double       drag_source_fx = 0.8;
    constexpr double       drag_source_fy = 0.6;
    constexpr double       drag_dest_fx   = 0.2;
    constexpr double       drag_dest_fy   = 0.3;
    constexpr input::Point drag_source_point{
        .x = 1'800,
        .y = 1'600,
    };
    constexpr input::Point drag_dest_point{
        .x = 1'200,
        .y = 1'300,
    };
    constexpr std::size_t drag_expected_op_count = 87U;
    constexpr std::size_t drag_sample_count =
        static_cast<std::size_t>( input::drag_cross_steps );
    constexpr std::array<std::int32_t, 6U> drag_arm_offsets{
        10,
        20,
        32,
        46,
        62,
        80,
    };
    constexpr std::array<input::Point, 4U> drag_nudges{
        input::Point{ .x = 0,  .y = 0},
        input::Point{ .x = 4,  .y = 3},
        input::Point{.x = -3,  .y = 2},
        input::Point{ .x = 2, .y = -2},
    };
    constexpr std::int32_t  drag_arm_y_lift                    = 6;
    constexpr std::uint32_t drag_curve_initial_wait_millis     = 150U;
    constexpr std::uint32_t drag_curve_press_wait_millis       = 200U;
    constexpr std::uint32_t drag_curve_step_wait_millis        = 60U;
    constexpr std::uint32_t drag_curve_arm_dwell_wait_millis   = 400U;
    constexpr std::uint32_t drag_curve_cross_dwell_wait_millis = 300U;
    constexpr std::uint32_t drag_curve_nudge_wait_millis       = 140U;
    constexpr std::uint32_t drag_curve_release_wait_millis     = 500U;
    constexpr std::int32_t  drag_rect_right =
        drag_rect.x + static_cast<std::int32_t>( drag_rect.width );
    constexpr std::int32_t drag_rect_bottom =
        drag_rect.y + static_cast<std::int32_t>( drag_rect.height );

    [[nodiscard]]
    input::Point
    translated( input::Point point,
                std::int32_t dx,
                std::int32_t dy ) noexcept
    {
        return input::Point{
            .x = point.x + dx,
            .y = point.y + dy,
        };
    }

    void
    expect_move( const test::Op& op,
                 input::Point    point )
    {
        const auto* const move = std::get_if<test::Move>( &op );
        ASSERT_NE( move, nullptr );
        EXPECT_EQ( move->p.x, point.x );
        EXPECT_EQ( move->p.y, point.y );
    }

    void
    expect_button( const test::Op& op,
                   std::uint8_t    code,
                   bool            expected_press,
                   bool            expected_clear_modifiers )
    {
        const auto* const button = std::get_if<test::Button>( &op );
        ASSERT_NE( button, nullptr );
        EXPECT_EQ( button->code, code );
        EXPECT_EQ( button->press, expected_press );
        EXPECT_EQ( button->clear_modifiers, expected_clear_modifiers );
    }

    void
    expect_sync( const test::Op& op )
    {
        ASSERT_NE( std::get_if<test::Sync>( &op ), nullptr );
    }

    void
    expect_wait( const test::Op& op,
                 std::uint32_t   millis )
    {
        const auto* const wait = std::get_if<test::Wait>( &op );
        ASSERT_NE( wait, nullptr );
        EXPECT_EQ( wait->millis, millis );
    }

    void
    expect_sample_in_bounds( input::Point point )
    {
        EXPECT_GE( point.x, drag_rect.x );
        EXPECT_LE( point.x, drag_rect_right );
        EXPECT_GE( point.y, drag_rect.y );
        EXPECT_LE( point.y, drag_rect_bottom );
    }

    void
    expect_samples_monotone_to_destination( const std::vector<input::Point>& samples )
    {
        for( std::size_t index = 1U; index < samples.size(); ++index )
        {
            EXPECT_LE( samples.at( index ).x, samples.at( index - 1U ).x );
            EXPECT_LE( samples.at( index ).y, samples.at( index - 1U ).y );
        }
    }

}    // namespace

TEST( Gesture,
      ClickFracRoundsToExactPixelAndClicks )
{
    test::FakeInputSink sink;

    input::click_frac( sink, click_rect, click_fx, click_fy );

    const std::vector<test::Op> expected{
        test::Move{ .p = click_point },
        test::Button{
                   .code            = left_button,
                   .press           = press,
                   .clear_modifiers = keep_modifiers,
                   },
        test::Button{
                   .code            = left_button,
                   .press           = release,
                   .clear_modifiers = keep_modifiers,
                   },
    };
    EXPECT_EQ( sink.ops(), expected );
}

TEST( Gesture,
      MenuItemOpensHeaderAndClicksIndexedItem )
{
    constexpr auto item_fy = menu_first_item_fy + ( static_cast<double>( menu_index ) *
                                                    menu_item_spacing_fy );
    EXPECT_DOUBLE_EQ( item_fy, menu_index_two_item_fy );

    test::FakeInputSink sink;

    input::menu_item( sink, menu_rect, menu_header_fx, menu_item_fx, menu_index );

    const std::vector<test::Op> expected{
        test::Activate{},
        test::Move{ .p = menu_header_point },
        test::Button{
                       .code            = left_button,
                       .press           = press,
                       .clear_modifiers = keep_modifiers,
                       },
        test::Button{
                       .code            = left_button,
                       .press           = release,
                       .clear_modifiers = keep_modifiers,
                       },
        test::Wait{ .millis = menu_open_wait_millis },
        test::Move{ .p = menu_item_point },
        test::Button{
                       .code            = left_button,
                       .press           = press,
                       .clear_modifiers = keep_modifiers,
                       },
        test::Button{
                       .code            = left_button,
                       .press           = release,
                       .clear_modifiers = keep_modifiers,
                       },
        test::Wait{ .millis = menu_item_wait_millis },
    };
    EXPECT_EQ( sink.ops(), expected );
}

TEST( Gesture,
      DragCurvePreservesPhasesAndSamplesPathProperties )
{
    test::FakeInputSink sink;

    input::drag_curve( sink,
                       drag_rect,
                       drag_source_fx,
                       drag_source_fy,
                       drag_dest_fx,
                       drag_dest_fy );

    const auto& ops = sink.ops();
    ASSERT_EQ( ops.size(), drag_expected_op_count );

    std::size_t index = 0U;
    expect_move( ops.at( index++ ), drag_source_point );
    expect_sync( ops.at( index++ ) );
    expect_wait( ops.at( index++ ), drag_curve_initial_wait_millis );
    expect_button( ops.at( index++ ), left_button, press, clear_modifiers );
    expect_wait( ops.at( index++ ), drag_curve_press_wait_millis );

    for( const auto offset : drag_arm_offsets )
    {
        expect_move( ops.at( index++ ),
                     translated( drag_source_point, offset, drag_arm_y_lift ) );
        expect_sync( ops.at( index++ ) );
        expect_wait( ops.at( index++ ), drag_curve_step_wait_millis );
    }

    expect_wait( ops.at( index++ ), drag_curve_arm_dwell_wait_millis );
    std::vector<input::Point> sampled_points;
    sampled_points.reserve( drag_sample_count );
    for( std::size_t sample = 0U; sample < drag_sample_count; ++sample )
    {
        const auto* const move = std::get_if<test::Move>( &ops.at( index++ ) );
        ASSERT_NE( move, nullptr );
        sampled_points.push_back( move->p );
        expect_sync( ops.at( index++ ) );
        expect_wait( ops.at( index++ ), drag_curve_step_wait_millis );
    }
    ASSERT_EQ( sampled_points.size(), drag_sample_count );
    EXPECT_EQ( sampled_points.front(), drag_source_point );
    EXPECT_EQ( sampled_points.back(), drag_dest_point );
    for( const auto point : sampled_points )
    {
        expect_sample_in_bounds( point );
    }
    expect_samples_monotone_to_destination( sampled_points );

    expect_wait( ops.at( index++ ), drag_curve_cross_dwell_wait_millis );
    for( const auto nudge : drag_nudges )
    {
        expect_move( ops.at( index++ ),
                     translated( drag_dest_point, nudge.x, nudge.y ) );
        expect_sync( ops.at( index++ ) );
        expect_wait( ops.at( index++ ), drag_curve_nudge_wait_millis );
    }

    expect_button( ops.at( index++ ), left_button, release, clear_modifiers );
    expect_wait( ops.at( index++ ), drag_curve_release_wait_millis );
    EXPECT_EQ( index, ops.size() );
}
