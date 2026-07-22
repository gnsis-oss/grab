#include "grab/event.hpp"
#include "grab/event_descriptor.hpp"
#include "kernel/events/event_bus.hpp"
#include "kernel/graph/state_manager.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>    // IWYU pragma: keep
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace grab::event
{
    namespace
    {

        constexpr std::uint64_t    unsetSequence = 0U;
        constexpr std::string_view openKey       = "open";
        constexpr std::string_view focusedKey    = "focused";
        constexpr std::string_view appKey        = "app";
        constexpr std::string_view pidKey        = "pid";
        constexpr std::string_view titleKey      = "title";

        using detail::WindowRecord;

        [[nodiscard]]
        bool
        is_empty_window( const grab::WindowChange& change ) noexcept
        {
            return change.app.empty() && !change.pid.valid() && change.title.empty();
        }

        [[nodiscard]]
        bool
        same_identity( const WindowRecord&       record,
                       const grab::WindowChange& change ) noexcept
        {
            return record.app == change.app && record.title == change.title;
        }

        [[nodiscard]]
        WindowRecord
        make_record( const grab::WindowChange& change )
        {
            return WindowRecord{
                .app   = change.app,
                .pid   = change.pid,
                .title = change.title,
            };
        }

        using OrderedJson = nlohmann::ordered_json;

        [[nodiscard]]
        OrderedJson
        window_to_json( const WindowRecord& window )
        {
            return OrderedJson{
                {  std::string{ appKey },             window.app},
                {  std::string{ pidKey }, window.pid.to_string()},
                {std::string{ titleKey },           window.title},
            };
        }

    }    // namespace

    StateManager::StateManager() = default;

    void
    StateManager::observe( const grab::Event& event )
    {
        const auto* change = std::get_if<grab::WindowChange>( &event.payload );
        if( change == nullptr )
        {
            return;
        }

        switch( event.kind )
        {
            case grab::EventKind::WindowCreated :
                {
                    auto existing =
                        std::ranges::find_if( open_windows_,
                                              [change]( const WindowRecord& record )
                                              {
                                                  return same_identity( record,
                                                                        *change );
                                              } );
                    if( existing == open_windows_.end() )
                    {
                        open_windows_.push_back( make_record( *change ) );
                    }
                    else
                    {
                        *existing = make_record( *change );
                    }
                    break;
                }
            case grab::EventKind::WindowClosed :
                {
                    const auto removed =
                        std::ranges::remove_if( open_windows_,
                                                [change]( const WindowRecord& record )
                                                {
                                                    return same_identity( record,
                                                                          *change );
                                                } );
                    open_windows_.erase( removed.begin(), removed.end() );

                    if( has_focused_window_ && same_identity( focused_, *change ) )
                    {
                        focused_            = WindowRecord{};
                        has_focused_window_ = false;
                    }
                    break;
                }
            case grab::EventKind::WindowFocusChanged :
                if( is_empty_window( *change ) )
                {
                    focused_            = WindowRecord{};
                    has_focused_window_ = false;
                }
                else
                {
                    focused_            = make_record( *change );
                    has_focused_window_ = true;
                }
                break;
            default :
                break;
        }
    }

    grab::Event
    StateManager::snapshot( double timestamp ) const
    {
        OrderedJson open = OrderedJson::array();
        for( const auto& window : open_windows_ )
        {
            open.push_back( window_to_json( window ) );
        }

        OrderedJson focused = has_focused_window_ ? window_to_json( focused_ )
                                                  : window_to_json( WindowRecord{} );

        const OrderedJson root{
            {   std::string{ openKey },    std::move( open )},
            {std::string{ focusedKey }, std::move( focused )},
        };

        return grab::Event{
            .timestamp = timestamp,
            .sequence  = unsetSequence,
            .kind      = grab::EventKind::StateSnapshot,
            .category  = grab::category_of( grab::EventKind::StateSnapshot ),
            .payload   = grab::Payload{ grab::StateSnapshot{
                .json = root.dump(),
            } },
        };
    }

    std::vector<grab::Event>
    StateManager::open_window_events( double timestamp ) const
    {
        std::vector<grab::Event> events;
        events.reserve( open_windows_.size() );

        for( const auto& window : open_windows_ )
        {
            events.push_back( grab::Event{
                .timestamp = timestamp,
                .sequence  = unsetSequence,
                .kind      = grab::EventKind::WindowCreated,
                .category  = grab::category_of( grab::EventKind::WindowCreated ),
                .payload   = grab::Payload{ grab::WindowChange{
                    .app        = window.app,
                    .pid        = window.pid,
                    .title      = window.title,
                    .prev_title = {},
                    .duration_s = 0.0,
                } },
            } );
        }

        return events;
    }

    void
    StateManager::publish_snapshot( grab::EventBus& bus,
                                    double          timestamp ) const
    {
        bus.publish( snapshot( timestamp ) );
    }

    std::size_t
    StateManager::open_window_count() const noexcept
    {
        return open_windows_.size();
    }

}    // namespace grab::event
