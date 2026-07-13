#pragma once

#include "eventgrab/v1/events.pb.h"
#include "grab/event.hpp"
#include "grab/event_descriptor.hpp"

#include <array>
#include <cstddef>
#include <optional>

namespace grab::transport
{

    struct ProtoKindRow
    {
            grab::EventKind          kind;
            eventgrab::v1::EventKind proto_kind;
    };

    [[nodiscard]]
    constexpr ProtoKindRow
    proto_kind_row( grab::EventKind          kind,
                    eventgrab::v1::EventKind proto_kind ) noexcept
    {
        return ProtoKindRow{
            .kind       = kind,
            .proto_kind = proto_kind,
        };
    }

    inline constexpr auto protoKindRows = std::to_array<ProtoKindRow>( {
        proto_kind_row( grab::EventKind::Unspecified,
                        eventgrab::v1::EVENT_KIND_UNSPECIFIED ),
        proto_kind_row( grab::EventKind::KeyDown, eventgrab::v1::INPUT_KEY_DOWN ),
        proto_kind_row( grab::EventKind::KeyUp, eventgrab::v1::INPUT_KEY_UP ),
        proto_kind_row( grab::EventKind::KeyCombo, eventgrab::v1::INPUT_KEY_COMBO ),
        proto_kind_row( grab::EventKind::MouseClick, eventgrab::v1::INPUT_MOUSE_CLICK ),
        proto_kind_row( grab::EventKind::MouseMove, eventgrab::v1::INPUT_MOUSE_MOVE ),
        proto_kind_row( grab::EventKind::IdleStart, eventgrab::v1::INPUT_IDLE_START ),
        proto_kind_row( grab::EventKind::IdleEnd, eventgrab::v1::INPUT_IDLE_END ),
        proto_kind_row( grab::EventKind::WindowFocusChanged,
                        eventgrab::v1::WINDOW_FOCUS_CHANGED ),
        proto_kind_row( grab::EventKind::WindowTitleChanged,
                        eventgrab::v1::WINDOW_TITLE_CHANGED ),
        proto_kind_row( grab::EventKind::WindowCreated, eventgrab::v1::WINDOW_CREATED ),
        proto_kind_row( grab::EventKind::WindowClosed, eventgrab::v1::WINDOW_CLOSED ),
        proto_kind_row( grab::EventKind::A11yButtonClicked,
                        eventgrab::v1::A11Y_BUTTON_CLICKED ),
        proto_kind_row( grab::EventKind::A11yMenuOpened,
                        eventgrab::v1::A11Y_MENU_OPENED ),
        proto_kind_row( grab::EventKind::A11yMenuClosed,
                        eventgrab::v1::A11Y_MENU_CLOSED ),
        proto_kind_row( grab::EventKind::A11yFocusChanged,
                        eventgrab::v1::A11Y_FOCUS_CHANGED ),
        proto_kind_row( grab::EventKind::A11yTextChanged,
                        eventgrab::v1::A11Y_TEXT_CHANGED ),
        proto_kind_row( grab::EventKind::A11yStateChanged,
                        eventgrab::v1::A11Y_STATE_CHANGED ),
        proto_kind_row( grab::EventKind::AppTabChanged, eventgrab::v1::APP_TAB_CHANGED ),
        proto_kind_row( grab::EventKind::AppContextUpdate,
                        eventgrab::v1::APP_CONTEXT_UPDATE ),
        proto_kind_row( grab::EventKind::BrowserTabSwitched,
                        eventgrab::v1::BROWSER_TAB_SWITCHED ),
        proto_kind_row( grab::EventKind::StateSnapshot, eventgrab::v1::STATE_SNAPSHOT ),
    } );

    struct ProtoCategoryRow
    {
            grab::EventCategory          category;
            eventgrab::v1::EventCategory proto_category;
    };

    [[nodiscard]]
    constexpr ProtoCategoryRow
    proto_category_row( grab::EventCategory          category,
                        eventgrab::v1::EventCategory proto_category ) noexcept
    {
        return ProtoCategoryRow{
            .category       = category,
            .proto_category = proto_category,
        };
    }

    inline constexpr auto protoCategoryRows = std::to_array<ProtoCategoryRow>( {
        proto_category_row( grab::EventCategory::Unspecified,
                            eventgrab::v1::EVENT_CATEGORY_UNSPECIFIED ),
        proto_category_row( grab::EventCategory::Input,
                            eventgrab::v1::EVENT_CATEGORY_INPUT ),
        proto_category_row( grab::EventCategory::Window,
                            eventgrab::v1::EVENT_CATEGORY_WINDOW ),
        proto_category_row( grab::EventCategory::Accessibility,
                            eventgrab::v1::EVENT_CATEGORY_ACCESSIBILITY ),
        proto_category_row( grab::EventCategory::Integration,
                            eventgrab::v1::EVENT_CATEGORY_INTEGRATION ),
        proto_category_row( grab::EventCategory::Browser,
                            eventgrab::v1::EVENT_CATEGORY_BROWSER ),
        proto_category_row( grab::EventCategory::State,
                            eventgrab::v1::EVENT_CATEGORY_STATE ),
    } );

    [[nodiscard]]
    constexpr eventgrab::v1::EventKind
    to_wire_kind( grab::EventKind kind ) noexcept
    {
        for( const auto& row : protoKindRows )
        {
            if( row.kind == kind )
            {
                return row.proto_kind;
            }
        }
        return eventgrab::v1::EVENT_KIND_UNSPECIFIED;
    }

    [[nodiscard]]
    constexpr std::optional<grab::EventKind>
    to_grab_kind( eventgrab::v1::EventKind kind ) noexcept
    {
        for( const auto& row : protoKindRows )
        {
            if( row.proto_kind == kind )
            {
                return row.kind;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]]
    constexpr eventgrab::v1::EventCategory
    to_wire_category( grab::EventCategory category ) noexcept
    {
        for( const auto& row : protoCategoryRows )
        {
            if( row.category == category )
            {
                return row.proto_category;
            }
        }
        return eventgrab::v1::EVENT_CATEGORY_UNSPECIFIED;
    }

    [[nodiscard]]
    constexpr std::optional<grab::EventCategory>
    to_grab_category( eventgrab::v1::EventCategory category ) noexcept
    {
        for( const auto& row : protoCategoryRows )
        {
            if( row.proto_category == category )
            {
                return row.category;
            }
        }
        return std::nullopt;
    }

    static_assert(
        []
        {
            if( protoKindRows.size() != grab::detail::eventDescriptors.size() )
            {
                return false;
            }

            for( std::size_t index = 0U; index < protoKindRows.size(); ++index )
            {
                if( protoKindRows[index].kind !=
                    grab::detail::eventDescriptors[index].kind )
                {
                    return false;
                }
            }
            return true;
        }()
    );

    static_assert(
        []
        {
            for( const auto& row : protoKindRows )
            {
                const auto kind = to_grab_kind( to_wire_kind( row.kind ) );
                if( !kind.has_value() || *kind != row.kind )
                {
                    return false;
                }
            }
            return true;
        }()
    );

    static_assert(
        []
        {
            if( protoCategoryRows.size() != grab::detail::categoryNames.size() )
            {
                return false;
            }

            for( const auto& row : protoCategoryRows )
            {
                const auto category =
                    to_grab_category( to_wire_category( row.category ) );
                if( !category.has_value() || *category != row.category )
                {
                    return false;
                }
            }

            for( const auto& row : protoKindRows )
            {
                const auto category = to_grab_category(
                    to_wire_category( grab::category_of( row.kind ) )
                );
                if( !category.has_value() || *category != grab::category_of( row.kind ) )
                {
                    return false;
                }
            }
            return true;
        }()
    );

}    // namespace grab::transport
