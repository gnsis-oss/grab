#include "drivers/desktop/x11/x11_input_correctness.hpp"
#include "drivers/desktop/x11/x11_runtime.hpp"
#include "spi/tree_source.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    using grab::drivers::desktop::x11::ModifierState;
    using grab::drivers::desktop::x11::ScratchKeycodeBackend;
    using grab::drivers::desktop::x11::ScratchKeycodeMapping;

    class FakeModifierState final : public ModifierState
    {
        public:

            [[nodiscard]]
            bool
            held( const std::uint8_t keycode ) const override
            {
                return held_[keycode];
            }

            [[nodiscard]]
            bool
            set( const std::uint8_t keycode,
                 const bool         press ) override
            {
                transitions.emplace_back( keycode, press );
                held_[keycode] = press;
                return true;
            }

            std::array<bool, 256U>                     held_{};
            std::vector<std::pair<std::uint8_t, bool>> transitions;
    };

    struct Replacement
    {
            std::uint8_t               keycode{};
            std::vector<std::uint32_t> keysyms;
    };

    class FakeScratchKeycodeBackend final : public ScratchKeycodeBackend
    {
        public:

            explicit FakeScratchKeycodeBackend(
                std::vector<ScratchKeycodeMapping> initial
            ) :
                mappings_{ std::move( initial ) }
            {
            }

            [[nodiscard]]
            grab::Result<std::vector<ScratchKeycodeMapping>>
            mappings() override
            {
                return mappings_;
            }

            [[nodiscard]]
            grab::Result<void>
            replace( const std::uint8_t                   keycode,
                     const std::span<const std::uint32_t> keysyms ) override
            {
                replacements.push_back( {
                    keycode,
                    std::vector<std::uint32_t>{ keysyms.begin(), keysyms.end() }
                } );
                for( auto& mapping : mappings_ )
                {
                    if( mapping.keycode == keycode )
                    {
                        mapping.keysyms.assign( keysyms.begin(), keysyms.end() );
                    }
                }
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            fence() override
            {
                ++fences;
                return {};
            }

            std::vector<ScratchKeycodeMapping> mappings_;
            std::vector<Replacement>           replacements;
            std::size_t                        fences{};
    };

}    // namespace

TEST( ModifierGuard,
      SnapshotsClearsAndRestoresHeldModifiers )
{
    constexpr std::uint8_t left_shift = 50U;
    constexpr std::uint8_t left_ctrl  = 37U;
    constexpr std::uint8_t left_alt   = 64U;
    FakeModifierState      state;
    state.held_[left_shift] = true;
    state.held_[left_alt]   = true;
    const std::array modifiers{ left_shift, left_ctrl, left_alt };

    {
        grab::drivers::desktop::x11::ModifierGuard guard{ state, modifiers };

        EXPECT_TRUE( guard.release_succeeded() );
        EXPECT_FALSE( state.held_[left_shift] );
        EXPECT_FALSE( state.held_[left_ctrl] );
        EXPECT_FALSE( state.held_[left_alt] );
        EXPECT_EQ( state.transitions,
                   ( std::vector<std::pair<std::uint8_t, bool>>{
                       {left_shift, false},
                       {  left_alt, false},
        } ) );

        ASSERT_TRUE( guard.restore() );
        EXPECT_TRUE( state.held_[left_shift] );
        EXPECT_FALSE( state.held_[left_ctrl] );
        EXPECT_TRUE( state.held_[left_alt] );
    }

    EXPECT_EQ( state.transitions.size(), 4U );
    const auto restored_shift = std::make_pair( left_shift, true );
    const auto restored_alt   = std::make_pair( left_alt, true );
    EXPECT_EQ( state.transitions[2], restored_shift );
    EXPECT_EQ( state.transitions[3], restored_alt );
}

TEST( InputCorrectnessSeatLane,
      ScopedAcquisitionsSerialize )
{
    using namespace std::chrono_literals;

    grab::drivers::desktop::x11::SeatLane lane;
    std::promise<void>                    attempting;
    std::promise<void>                    acquired;
    auto                                  attempting_future = attempting.get_future();
    auto                                  acquired_future   = acquired.get_future();
    std::thread                           contender;

    {
        const auto first = lane.acquire();
        contender        = std::thread{ [&lane, &attempting, &acquired]
                                        {
                                     attempting.set_value();
                                     const auto second = lane.acquire();
                                     acquired.set_value();
                                        } };

        EXPECT_EQ( attempting_future.wait_for( 1s ), std::future_status::ready );
        EXPECT_EQ( acquired_future.wait_for( 20ms ), std::future_status::timeout );
    }
    EXPECT_EQ( acquired_future.wait_for( 1s ), std::future_status::ready );
    contender.join();
}

TEST( ScratchKeycode,
      FindsFirstMappingWhoseKeysymsAreUnused )
{
    const std::vector<ScratchKeycodeMapping> mappings{
        { 8U, { 0X00'61U, 0U }},
        {42U,       { 0U, 0U }},
        {43U,       { 0U, 0U }},
    };

    EXPECT_EQ( grab::drivers::desktop::x11::find_unused_keycode( mappings ),
               std::optional<std::uint8_t>{ 42U } );

    const std::array used_only{
        ScratchKeycodeMapping{8U, { 0X00'61U }},
        ScratchKeycodeMapping{9U, { 0X00'62U }},
    };
    EXPECT_FALSE(
        grab::drivers::desktop::x11::find_unused_keycode( used_only ).has_value()
    );
}

TEST( ScratchKeycodePool,
      SharesLoanAndRevertsOriginalMappingAfterLastRestore )
{
    constexpr std::uint8_t  scratch_keycode = 42U;
    constexpr std::uint32_t keysym          = 0X20'ACU;
    auto                    backend =
        std::make_unique<FakeScratchKeycodeBackend>( std::vector<ScratchKeycodeMapping>{
            {             8U, { 0X00'61U, 0U }},
            {scratch_keycode,       { 0U, 0U }},
    } );
    auto* const                                     backend_view = backend.get();
    grab::drivers::desktop::x11::ScratchKeycodePool pool{ std::move( backend ) };

    auto                                            first = pool.loan( keysym );
    ASSERT_TRUE( first.has_value() );
    EXPECT_EQ( first->keycode(), scratch_keycode );
    ASSERT_EQ( backend_view->replacements.size(), 1U );

    auto second = pool.loan( keysym );
    ASSERT_TRUE( second.has_value() );
    EXPECT_EQ( second->keycode(), scratch_keycode );
    EXPECT_EQ( backend_view->replacements.size(), 1U );

    ASSERT_TRUE( first->restore().has_value() );
    EXPECT_EQ( backend_view->replacements.size(), 1U );
    ASSERT_TRUE( second->restore().has_value() );
    ASSERT_EQ( backend_view->replacements.size(), 2U );
    EXPECT_EQ( backend_view->replacements.back().keycode, scratch_keycode );
    const std::vector<std::uint32_t> original_mapping{ 0U, 0U };
    EXPECT_EQ( backend_view->replacements.back().keysyms, original_mapping );
    EXPECT_EQ( backend_view->fences, 2U );
}

TEST( ScratchKeycodePool,
      DISABLED_ConnectionBackendRequiresXServer )
{
    GTEST_SKIP() << "Requires a live X11 display";
}

TEST( X11InputCorrectness,
      PointerRouteReservesMatchTarget )
{
    if( std::getenv( "DISPLAY" ) == nullptr )
    {
        GTEST_SKIP() << "Requires a live X11 display";
    }

    constexpr std::uint32_t                 snapshot_tree = 1U;
    constexpr std::uint64_t                 fallback_node = 1U;
    constexpr grab::NodeGeneration          fallback_generation{ 1U };
    constexpr std::string_view              pointer_route_name{ "x11.pointer" };

    grab::OperationContext                  context;
    grab::drivers::desktop::x11::X11Runtime runtime;
    ASSERT_TRUE( runtime.start( context ).has_value() );

    const auto                 descriptors = runtime.routes();
    std::optional<std::size_t> pointer_route_index;
    for( std::size_t index{}; index < descriptors.size(); ++index )
    {
        if( descriptors[index].name == pointer_route_name )
        {
            pointer_route_index = index;
            break;
        }
    }
    ASSERT_TRUE( pointer_route_index.has_value() );

    auto* const route = runtime.action_route( *pointer_route_index );
    ASSERT_NE( route, nullptr );

    auto* const tree_source = runtime.tree_source();
    ASSERT_NE( tree_source, nullptr );
    auto snapshot = tree_source->snapshot( snapshot_tree, context );
    ASSERT_TRUE( snapshot.has_value() );

    const auto nodes = snapshot->nodes();
    const auto node  = nodes.empty() ? fallback_node : nodes.front().id.value;
    const auto generation =
        nodes.empty() ? fallback_generation : nodes.front().generation;
    const grab::WidgetRef widget{
        .runtime    = snapshot->runtime,
        .tree       = snapshot->tree,
        .epoch      = snapshot->epoch,
        .node       = node,
        .generation = generation,
    };
    const grab::spi::ActionRequest request{
        .verb = grab::spi::ActionVerb::Click,
        .target =
            grab::Match{
                        .ref                = widget,
                        .mode               = grab::ConsistencyMode::Live,
                        .snapshot_revision  = snapshot->revision,
                        .matched_predicates = {},
                        .provenance         = {},
                        },
        .text = {},
    };

    {
        const auto reservation = route->reserve( request, context );
        EXPECT_TRUE( reservation.has_value() );
    }

    EXPECT_TRUE( runtime.stop().has_value() );
}
