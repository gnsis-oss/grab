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
            grab::EventKind            kind;
            eventgrab::v1::EventKind   proto_kind;
    };

    inline constexpr std::array<ProtoKindRow, 22> protoKindRows{
        {
         { .kind       = grab::EventKind::Unspecified,
           .proto_kind = eventgrab::v1::EVENT_KIND_UNSPECIFIED },
         { .kind       = grab::EventKind::KeyDown,
           .proto_kind = eventgrab::v1::INPUT_KEY_DOWN },
         { .kind       = grab::EventKind::KeyUp,
           .proto_kind = eventgrab::v1::INPUT_KEY_UP },
         { .kind       = grab::EventKind::KeyCombo,
           .proto_kind = eventgrab::v1::INPUT_KEY_COMBO },
         { .kind       = grab::EventKind::MouseClick,
           .proto_kind = eventgrab::v1::INPUT_MOUSE_CLICK },
         { .kind       = grab::EventKind::MouseMove,
           .proto_kind = eventgrab::v1::INPUT_MOUSE_MOVE },
         { .kind       = grab::EventKind::IdleStart,
           .proto_kind = eventgrab::v1::INPUT_IDLE_START },
         { .kind       = grab::EventKind::IdleEnd,
           .proto_kind = eventgrab::v1::INPUT_IDLE_END },
         { .kind       = grab::EventKind::WindowFocusChanged,
           .proto_kind = eventgrab::v1::WINDOW_FOCUS_CHANGED },
         { .kind       = grab::EventKind::WindowTitleChanged,
           .proto_kind = eventgrab::v1::WINDOW_TITLE_CHANGED },
         { .kind       = grab::EventKind::WindowCreated,
           .proto_kind = eventgrab::v1::WINDOW_CREATED },
         { .kind       = grab::EventKind::WindowClosed,
           .proto_kind = eventgrab::v1::WINDOW_CLOSED },
         { .kind       = grab::EventKind::A11yButtonClicked,
           .proto_kind = eventgrab::v1::A11Y_BUTTON_CLICKED },
         { .kind       = grab::EventKind::A11yMenuOpened,
           .proto_kind = eventgrab::v1::A11Y_MENU_OPENED },
         { .kind       = grab::EventKind::A11yMenuClosed,
           .proto_kind = eventgrab::v1::A11Y_MENU_CLOSED },
         { .kind       = grab::EventKind::A11yFocusChanged,
           .proto_kind = eventgrab::v1::A11Y_FOCUS_CHANGED },
         { .kind       = grab::EventKind::A11yTextChanged,
           .proto_kind = eventgrab::v1::A11Y_TEXT_CHANGED },
         { .kind       = grab::EventKind::A11yStateChanged,
           .proto_kind = eventgrab::v1::A11Y_STATE_CHANGED },
         { .kind       = grab::EventKind::AppTabChanged,
           .proto_kind = eventgrab::v1::APP_TAB_CHANGED },
         { .kind       = grab::EventKind::AppContextUpdate,
           .proto_kind = eventgrab::v1::APP_CONTEXT_UPDATE },
         { .kind       = grab::EventKind::BrowserTabSwitched,
           .proto_kind = eventgrab::v1::BROWSER_TAB_SWITCHED },
         { .kind       = grab::EventKind::StateSnapshot,
           .proto_kind = eventgrab::v1::STATE_SNAPSHOT },
         }
    };

    struct ProtoCategoryRow
    {
            grab::EventCategory            category;
            eventgrab::v1::EventCategory   proto_category;
    };

    inline constexpr std::array<ProtoCategoryRow, 7> protoCategoryRows{
        {
         { .category       = grab::EventCategory::Unspecified,
           .proto_category = eventgrab::v1::EVENT_CATEGORY_UNSPECIFIED },
         { .category       = grab::EventCategory::Input,
           .proto_category = eventgrab::v1::EVENT_CATEGORY_INPUT },
         { .category       = grab::EventCategory::Window,
           .proto_category = eventgrab::v1::EVENT_CATEGORY_WINDOW },
         { .category       = grab::EventCategory::Accessibility,
           .proto_category = eventgrab::v1::EVENT_CATEGORY_ACCESSIBILITY },
         { .category       = grab::EventCategory::Integration,
           .proto_category = eventgrab::v1::EVENT_CATEGORY_INTEGRATION },
         { .category       = grab::EventCategory::Browser,
           .proto_category = eventgrab::v1::EVENT_CATEGORY_BROWSER },
         { .category       = grab::EventCategory::State,
           .proto_category = eventgrab::v1::EVENT_CATEGORY_STATE },
         }
    };

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
            if( protoCategoryRows.size() != 7U )
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
                const auto category =
                    to_grab_category( to_wire_category( grab::category_of( row.kind ) ) );
                if( !category.has_value() ||
                    *category != grab::category_of( row.kind ) )
                {
                    return false;
                }
            }
            return true;
        }()
    );

}    // namespace grab::transport
