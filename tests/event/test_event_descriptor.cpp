#include "frontends/grpc/codec.hpp"
#include "grab/event_descriptor.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
// clang-format on

namespace
{

    constexpr auto descriptorCount              = 29U;
    constexpr auto keyDownKind                  = grab::EventKind::KeyDown;
    constexpr auto mouseButtonDownKind          = grab::EventKind::MouseButtonDown;
    constexpr auto mouseButtonUpKind            = grab::EventKind::MouseButtonUp;
    constexpr auto windowCreatedKind            = grab::EventKind::WindowCreated;
    constexpr auto a11yTextChangedKind          = grab::EventKind::A11yTextChanged;
    constexpr auto appContextUpdateKind         = grab::EventKind::AppContextUpdate;
    constexpr auto stateSnapshotKind            = grab::EventKind::StateSnapshot;
    constexpr auto nodeAddedKind                = grab::EventKind::NodeAdded;
    constexpr auto nodeChangedKind              = grab::EventKind::NodeChanged;
    constexpr auto activeChildChangedKind       = grab::EventKind::ActiveChildChanged;
    constexpr auto inputCategory                = grab::EventCategory::Input;
    constexpr auto windowCategory               = grab::EventCategory::Window;
    constexpr auto a11yCategory                 = grab::EventCategory::Accessibility;
    constexpr auto integrationCategory          = grab::EventCategory::Integration;
    constexpr auto stateCategory                = grab::EventCategory::State;
    constexpr std::string_view appContextUpdate = "app.context_update";
    constexpr std::string_view appTabChanged    = "app.tab_changed";
    constexpr std::string_view inputKeyDown     = "input.key_down";
    constexpr std::string_view inputMouseButtonDown = "input.mouse_button_down";
    constexpr std::string_view inputMouseButtonUp   = "input.mouse_button_up";
    constexpr std::string_view unspecified          = "unspecified";
    constexpr std::string_view flatContextUpdate    = "context_update";
    constexpr std::string_view unknownType          = "does.not.exist";
    constexpr std::string_view nodeAdded            = "node.added";
    constexpr std::string_view activeChildChanged   = "active_child.changed";
    constexpr auto             coalesceClass        = grab::CoalescingClass::Coalesce;
    constexpr auto             noReplayPolicy       = grab::ReplayPolicy::None;
    constexpr auto             currentSetPolicy     = grab::ReplayPolicy::CurrentSet;
    constexpr double           mousePositionX       = 321.25;
    constexpr double           mousePositionY       = 654.5;
    constexpr std::uint32_t    mouseButton          = 1U;
    constexpr std::uint32_t    mouseSpace           = 17U;
    constexpr std::string_view mouseButtonName      = "left";
    constexpr std::array       mouseButtonKinds{
        mouseButtonDownKind,
        mouseButtonUpKind,
    };

    static_assert( grab::detail::eventDescriptors.size() == descriptorCount );
    static_assert( grab::category_of( keyDownKind ) == inputCategory );
    static_assert( grab::category_of( mouseButtonDownKind ) == inputCategory );
    static_assert( grab::category_of( mouseButtonUpKind ) == inputCategory );
    static_assert( grab::category_of( windowCreatedKind ) == windowCategory );
    static_assert( grab::category_of( a11yTextChangedKind ) == a11yCategory );
    static_assert( grab::category_of( appContextUpdateKind ) == integrationCategory );
    static_assert( grab::category_of( stateSnapshotKind ) == stateCategory );

}    // namespace

TEST( EventDescriptor,
      MapsKindToWireName )
{
    EXPECT_EQ( grab::wire_name( appContextUpdateKind ), appContextUpdate );
    EXPECT_EQ( grab::wire_name( keyDownKind ), inputKeyDown );
    EXPECT_EQ( grab::wire_name( mouseButtonDownKind ), inputMouseButtonDown );
    EXPECT_EQ( grab::wire_name( mouseButtonUpKind ), inputMouseButtonUp );
}

TEST( EventDescriptor,
      MapsWireNameToKind )
{
    EXPECT_EQ( grab::wire_kind( appContextUpdate ), appContextUpdateKind );
    EXPECT_EQ( grab::wire_kind( appTabChanged ), grab::EventKind::AppTabChanged );
    EXPECT_EQ( grab::wire_kind( inputMouseButtonDown ), mouseButtonDownKind );
    EXPECT_EQ( grab::wire_kind( inputMouseButtonUp ), mouseButtonUpKind );
}

TEST( EventDescriptor,
      RejectsDroppedFlatAliases )
{
    EXPECT_EQ( grab::wire_kind( flatContextUpdate ), std::nullopt );
}

TEST( EventDescriptor,
      RejectsUnknownType )
{
    EXPECT_EQ( grab::wire_kind( unknownType ), std::nullopt );
    EXPECT_EQ( grab::wire_name( grab::EventKind::Unspecified ), unspecified );
}

TEST( EventDescriptor,
      RoundTripsEveryKind )
{
    for( const auto& descriptor : grab::detail::eventDescriptors )
    {
        EXPECT_EQ( grab::wire_kind( descriptor.wire_name ), descriptor.kind );
        EXPECT_EQ( grab::wire_name( descriptor.kind ), descriptor.wire_name );
    }
}

TEST( EventDescriptor,
      MouseButtonKindsRoundTripThroughTransportCodec )
{
    const grab::SpacePoint position{
        .x     = mousePositionX,
        .y     = mousePositionY,
        .space = grab::CoordinateSpaceId{ mouseSpace },
    };
    const grab::MouseButton payload{
        .button   = mouseButton,
        .name     = std::string{ mouseButtonName },
        .position = position,
    };

    for( const auto kind : mouseButtonKinds )
    {
        const grab::Event event{
            .kind     = kind,
            .category = inputCategory,
            .payload  = payload,
        };

        const auto wire = grab::transport::to_wire( event );
        ASSERT_TRUE( wire.has_value() );

        const auto decoded = grab::transport::from_wire( *wire );
        ASSERT_TRUE( decoded.has_value() );
        EXPECT_EQ( decoded->kind, kind );
        EXPECT_EQ( decoded->category, inputCategory );
        ASSERT_TRUE( std::holds_alternative<grab::MouseButton>( decoded->payload ) );

        const auto& decoded_payload = std::get<grab::MouseButton>( decoded->payload );
        EXPECT_EQ( decoded_payload.button, mouseButton );
        EXPECT_EQ( decoded_payload.name, mouseButtonName );
        ASSERT_TRUE( decoded_payload.position.has_value() );
        EXPECT_DOUBLE_EQ( decoded_payload.position->x, mousePositionX );
        EXPECT_DOUBLE_EQ( decoded_payload.position->y, mousePositionY );
        EXPECT_EQ( decoded_payload.position->space.value, mouseSpace );
    }
}

TEST( EventDescriptor,
      GraphEventRowsExist )
{
    EXPECT_EQ( grab::wire_name( nodeAddedKind ), nodeAdded );
    EXPECT_EQ( grab::wire_name( activeChildChangedKind ), activeChildChanged );
    EXPECT_EQ( grab::coalescing_class_of( nodeChangedKind ), coalesceClass );
    EXPECT_EQ( grab::replay_policy_of( nodeAddedKind ), noReplayPolicy );
    EXPECT_EQ( grab::replay_policy_of( windowCreatedKind ), currentSetPolicy );
}
